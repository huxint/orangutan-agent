## [2026-06-09 01:39] | Task: automation agent prompt bridge

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `local CLI`
- Linked plan: `docs/exec-plans/active/2026-06-07-automation-cron-category.md`

### User Query

> Continue the highest-value current slice in `orangutan-refactor`, follow the
> full repo workflow, and keep shipping slices end to end instead of stopping
> at partial code or STATUS churn.

### Changes Overview

- Areas: `oran-bootstrap`, automation/bootstrap integration docs, slice
  release/status tracking.
- Key actions: added a bootstrap-owned automation prompt bridge that builds one
  `AgentPromptRunner` per durable automation job execution, added focused
  bootstrap coverage for session continuity, per-job agent overlay selection,
  fail-closed approval behavior, and runtime-level cron/triggered execution,
  and documented the new noninteractive runner binding option plus the bridge
  ownership boundary.

### Design Intent

Slice 220 made stored automation prompts executable through injected handler
adapters, but the highest-value remaining gap was still at the bootstrap
boundary: there was no small owned seam that could run those prompts through
the real configured-route `AgentPromptRunner` without making automation own
provider/runtime assembly construction or background lifecycle. This slice
closes exactly that gap. The bridge deliberately constructs a fresh
`AgentPromptRunner` per automation job execution because automation jobs carry
per-job `agent_key`, prompt-overlay, permission-overlay, and persisted-session
identity semantics that would be incorrect if one runner instance were reused
across unrelated jobs. The bridge also defaults the new
`bind_operator_prompt_sink` option off so noninteractive automation asks stay
fail-closed instead of reading terminal stdin.

### Files Modified

- `include/oran/bootstrap.hpp`
- `include/oran/bootstrap/automation_prompt_runner.hpp`
- `include/oran/bootstrap/prompt_runner.hpp`
- `src/oran-bootstrap/automation_prompt_runner.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `src/oran-bootstrap/prompt_runner.cpp`
- `tests/bootstrap/test_automation_prompt_runner.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumps the snapshot to slice 221 and records the completed
  bootstrap automation prompt bridge boundary.
- `docs/ARCHITECTURE.md` — records the new bootstrap bridge and updated
  automation/bootstrap ownership boundary.
- `docs/design-docs/bootstrap-runtime.md` — documents the bridge API,
  per-job runner construction, and fail-closed noninteractive approval policy.
- `docs/design-docs/automation-runtime.md` — documents bootstrap-owned
  `AgentPromptRunner` wiring above the automation-owned prompt adapter seam.
- `docs/product-specs/0006-automation.md` — updates shipped prework and current
  implementation status for the bridge slice.
- `docs/QUALITY_SCORE.md` — updates bootstrap/automation coverage notes and
  `test-bootstrap` counts.
- `docs/exec-plans/active/2026-06-07-automation-cron-category.md` — records
  slice-221 progress and validation coverage.
- `docs/releases/feature-release-notes.md` — adds the user-facing release note
  for the new bridge.

### Validation

- Commands run:
  - `git diff --check`
  - `xmake build test-bootstrap`
  - `xmake run test-bootstrap`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `make ci`
- Tests added/changed: added focused bootstrap coverage for durable
  session-history reuse across automation runs, per-job configured-agent
  overlay application, fail-closed `ask` behavior without an operator sink, and
  runtime-level cron/triggered execution through the bridge.
- Bench impact: none; the slice adds bootstrap wiring and focused tests only.
- Compile-budget delta: one small `oran-bootstrap` implementation file, one
  lightweight public header, and a narrow `AgentPromptRunnerOptions` flag.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-06`
