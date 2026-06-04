## [2026-06-04 10:39] | Task: skill activation events

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `local shell in repository checkout`
- Linked plan: none; this was a small follow-up slice under the documented
  skill activation-policy path.

### User Query

> Start the next slice after reading and understanding the project architecture
> and current implementation progress.

### Changes Overview

- Areas: `oran-skill`, bootstrap prompt runner, skill docs.
- Key actions: added `skill::SkillActivationEvent` plus
  `skill::skill_activation_events_from_transcript(...)`, refactored
  `skill::active_skills_from_transcript(...)` to net that shared event stream,
  and changed `AgentPromptRunner` to persist session skill activation updates
  by consuming the shared extractor instead of owning a duplicate transcript
  parser. The binary banner moved to slice 149.

### Design Intent

Slice 148 made skill activation state durable, but the persistence path still
had bootstrap-local transcript parsing that duplicated `oran-skill` policy
logic. Slice 149 makes the ordered activation/deactivation event stream a
public `oran-skill` concept. That keeps section-4 rendering deterministic and
unchanged, while giving future CLI/web/channel/automation runtime owners the
same source-of-truth when they need to persist or replay skill activation
events.

### Files Modified

- `include/oran/skill/catalog.hpp`
- `src/oran-skill/catalog.cpp`
- `src/oran-bootstrap/prompt_runner.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/skill/test_catalog.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/ARCHITECTURE.md` — updated the `oran-skill` and `oran-prompt`
  inventory rows for the public event extractor.
- `docs/design-docs/agent-platform.md` — recorded the shared extractor in
  section-4 prompt assembly ownership.
- `docs/design-docs/bootstrap-runtime.md` — recorded bootstrap's use of the
  shared extractor for session persistence.
- `docs/product-specs/0009-skills.md` — updated skill activation/deactivation
  status and coverage notes.
- `docs/rules/prompt-design.md` — updated the section-4 slice summary through
  slice 149.
- `docs/QUALITY_SCORE.md` — refreshed the skill test count and next-step
  wording.
- `docs/STATUS.md` — moved the snapshot to slice 149 and recorded focused
  validation.

### Validation

- Commands run:
  - `xmake build test-skill`
  - `xmake run test-skill`
  - `xmake build test-bootstrap`
  - `xmake run test-bootstrap`
- Tests added/changed: `tests/skill/test_catalog.cpp` now covers suffix
  extraction, ignored non-skill/error results, and out-of-range suffix handling
  for `skill_activation_events_from_transcript(...)`.
- Bench impact: none; this is a small parser ownership refactor on an existing
  prompt-boundary path.
- Compile-budget delta: not measured; touched TUs remain within the existing
  focused build path.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: none; this is an internal runtime API boundary.
