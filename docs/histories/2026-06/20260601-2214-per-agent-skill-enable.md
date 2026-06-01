## [2026-06-01 22:14] | Task: per-agent skill enablement

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: local xmake / GCC 16.1 release targets
- Linked plan: none; `docs/STATUS.md` routes the prompt-runtime arc after slice 138.

### User Query

> Continue the prompt-runtime slice work with docs kept in sync and preserve the current WIP.

### Changes Overview

- Areas: `oran-config`, `oran-bootstrap`, prompt-runtime docs.
- Key actions:
  - Added typed parsing for optional `agents.<name>.skills_enabled`, validating it as an array of non-empty skill names.
  - Added `AgentPromptRunnerOptions::agent_config_name` so embedders can select the agent config used for prompt/runtime fields separately from the permission overlay when needed.
  - `AgentPromptRunner` now filters the workspace skill snapshot through the selected agent's allowlist before replacing section 4 and before serving `skill.invoke`.
  - Filtered-out skills behave like unloaded skills at invocation time, preserving the existing model-repairable `skill_not_loaded` path.

### Design Intent

This keeps skill selection at the bootstrap runner boundary, where the workspace skill snapshot and `skill.invoke` callback already meet. The lower-level `oran-skill` loader still snapshots all valid workspace files, and `oran-tool` remains independent from skill config. The slice intentionally does not overload the current `--agent` CLI flag, which is documented for `--explain-rules`; a user-facing runtime agent selector should land as its own CLI/config slice.

### Files Modified

- `include/oran/config/config.hpp`
- `src/oran-config/config.cpp`
- `include/oran/bootstrap/prompt_runner.hpp`
- `src/oran-bootstrap/prompt_runner.cpp`
- `tests/config/test_config.cpp`
- `tests/bootstrap/test_prompt_runner.cpp`
- `config.example.json`

### Docs Updated In This PR (Prime Directive -- see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` -- slice 139 snapshot, last-history pointer, and focused validation.
- `docs/ARCHITECTURE.md` -- config inventory and typed config surface notes.
- `docs/QUALITY_SCORE.md` -- config/skill rows and downstream runtime-agent-selector gap.
- `docs/design-docs/agent-platform.md` -- section-4 per-agent filtering boundary.
- `docs/design-docs/bootstrap-runtime.md` -- runner-owned allowlist filtering.
- `docs/design-docs/secrets-and-state.md` -- `agents.<name>.skills_enabled` config shape.
- `docs/product-specs/0009-skills.md` -- v1 per-agent enablement status and v1.1 follow-up.
- `docs/rules/prompt-design.md` -- section-4 allowlist filtering cache boundary.
- `docs/releases/feature-release-notes.md` -- user-visible embedder/runtime-surface note.

### Validation

- Commands run:
  - `xmake run test-config`
  - `timeout 60s xmake run -y test-bootstrap`
- Tests added/changed:
  - Added `test-bootstrap` coverage proving a selected agent allowlist filters both the rendered skill catalog and `skill.invoke` document snapshot.
  - Added `test-bootstrap` coverage proving an explicitly empty allowlist keeps loaded skills out of both section 4 and `skill.invoke`.
  - Extended `test-config` coverage for parsed allowlists and malformed empty skill names.
- Bench impact: no new bench; this is a one-shot config parse and prompt-boundary filter over an already bounded snapshot.
- Compile-budget delta: no budget cap change; the new code uses existing config/skill/bootstrap surfaces and keeps filtering in private bootstrap implementation.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md`.
