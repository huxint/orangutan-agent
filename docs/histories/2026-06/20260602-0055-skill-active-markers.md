## [2026-06-02 00:55] | Task: Skill Active Markers

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: none

### User Query

Continue the prompt-runtime skill arc after per-agent prompt overlays, keeping
skill activation visible without moving invoked skill bodies into stable prompt
sections.

### Changes Overview

- Areas: skill catalog rendering, bootstrap prompt runner, tests, prompt-runtime
  docs, release/status tracking.
- Key actions: add `skill::ActiveSkill` markers and versioned
  `skill.invoke` activation `data_json`; derive active markers from successful
  transcript tool results; filter markers through the current loaded/allowed
  skill snapshot; and bump the binary slice tag to `2.0.0-slice142`.

### Design Intent

The body of an invoked skill remains ordinary conversation-tail tool-result text,
so the active turn's cached prefix does not change. The next prompt can still
show stable section-4 state by reading the structured activation record produced
by the tool path. Bootstrap owns the filtering step because it already combines
the session transcript, current workspace skill snapshot, and selected-agent
allowlist.

### Files Modified

- `include/oran/skill/catalog.hpp`
- `src/oran-skill/catalog.cpp`
- `src/oran-bootstrap/prompt_runner.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/skill/test_catalog.cpp`
- `tests/bootstrap/test_prompt_runner.cpp`
- `docs/STATUS.md`
- `docs/ARCHITECTURE.md`
- `docs/design-docs/agent-platform.md`
- `docs/design-docs/bootstrap-runtime.md`
- `docs/product-specs/0009-skills.md`
- `docs/QUALITY_SCORE.md`
- `docs/releases/feature-release-notes.md`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` - slice 142 snapshot, last-history pointer, and validation.
- `docs/ARCHITECTURE.md` - library inventory now names active marker extraction
  and bootstrap filtering.
- `docs/design-docs/agent-platform.md` - section-4 active marker ownership.
- `docs/design-docs/bootstrap-runtime.md` - prompt-boundary transcript/snapshot
  filtering and `skill.invoke` activation metadata.
- `docs/product-specs/0009-skills.md` - skill activation status and acceptance
  criteria.
- `docs/QUALITY_SCORE.md` - skill/bootstrap test counts and current state.
- `docs/releases/feature-release-notes.md` - user-visible slice 142 row.

### Validation

- Commands run:
  - `xmake run -y test-skill`
  - `timeout 60s xmake run -y test-bootstrap`
  - `make ci`
- Tests added/changed: `test-skill` covers deterministic active marker
  rendering, duplicate active marker rejection, activation metadata round-trip,
  and transcript extraction. `test-bootstrap` proves a successful `skill.invoke`
  result marks the skill active only on the next prompt, while the active turn
  receives the skill body through normal tool-result text.
- Bench impact: no new bench; this is prompt-boundary metadata extraction and
  catalog rendering over the already-loaded skill snapshot, not a new hot-path
  algorithm.
- Compile-budget delta: no new target, dependency, or heavy public include.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md`
