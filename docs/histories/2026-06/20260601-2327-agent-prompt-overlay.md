## [2026-06-01 23:27] | Task: Agent Prompt Overlay

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: none

### User Query

Continue the prompt-runtime arc after runtime agent selection and keep the
repository docs in sync with the shipped per-agent prompt-overlay behavior.

### Changes Overview

- Areas: config, bootstrap prompt runner, prompt-runtime docs, tests, and
  release/status tracking.
- Key actions: add typed `agents.<name>.prompt_overlay` parsing to
  `oran-config`; have `AgentPromptRunner` fill stable section-6
  `per_agent_overlay` bytes from the selected agent config unless the caller
  supplied exact overlay text; keep the same selected-agent lookup for
  `skills_enabled`; and bump the binary slice tag to `2.0.0-slice141`.

### Design Intent

Section 6 already exists as the stable per-agent overlay in the prompt cache
contract, so this slice makes the configuration source explicit instead of
adding another prompt section. Bootstrap owns the handoff because it is the
boundary that already combines loaded config, selected agent identity, skill
allowlists, permission materialization, and loop inputs. Exact caller-supplied
`per_agent_overlay` remains authoritative for tests and embedders.

### Files Modified

- `config.example.json`
- `include/oran/config/config.hpp`
- `src/oran-config/config.cpp`
- `tests/config/test_config.cpp`
- `include/oran/bootstrap/prompt_runner.hpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `src/oran-bootstrap/prompt_runner.cpp`
- `tests/bootstrap/test_prompt_runner.cpp`
- `docs/STATUS.md`
- `docs/design-docs/agent-platform.md`
- `docs/design-docs/bootstrap-runtime.md`
- `docs/design-docs/secrets-and-state.md`
- `docs/QUALITY_SCORE.md`
- `docs/releases/feature-release-notes.md`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` - slice 141 snapshot, last-history pointer, and validation.
- `docs/design-docs/agent-platform.md` - section-6 ownership now names
  `agents.<name>.prompt_overlay`.
- `docs/design-docs/bootstrap-runtime.md` - selected agent config now feeds
  prompt overlays and skill allowlists.
- `docs/design-docs/secrets-and-state.md` - config field shape and bootstrap
  consumption documented.
- `docs/QUALITY_SCORE.md` - config/bootstrap/test current-state rows.
- `docs/releases/feature-release-notes.md` - user-visible slice 141 row.

### Validation

- Commands run:
  - `xmake run -y test-config`
  - `xmake run -y test-bootstrap`
  - `make ci`
- Tests added/changed: `test-config` covers typed parsing and malformed
  `prompt_overlay` rejection; `test-bootstrap` proves the selected agent
  overlay appears in the stable prompt prefix and remains byte-identical across
  provider iterations.
- Bench impact: no new bench; this is config parsing plus one runner setup-time
  string handoff, not a hot-path renderer change.
- Compile-budget delta: no new target, dependency, or heavy public include.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md`
