## [2026-06-02 01:39] | Task: Skill Activation Policy

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: none — this was the single-slice follow-up named by
  `docs/STATUS.md` after slice 142.

### User Query

Continue deeply grounded iteration on the current prompt-runtime arc before
further code implementation, keeping the next step tied to the documented
architecture and progress.

### Changes Overview

- Areas: `oran-skill`, bootstrap prompt runner, tests, prompt-runtime docs,
  release/status tracking.
- Key actions: add `skill::ActivationPolicy` and
  `skill::resolve_active_skills(...)`; move active-marker loaded/allowed
  filtering behind the `oran-skill` policy boundary; keep bootstrap responsible
  only for workspace snapshots and selected-agent allowlists; and bump the
  binary slice tag to `2.0.0-slice143`.

### Design Intent

Slice 142 made active skill markers visible, but the policy was still implicit:
bootstrap derived markers from the transcript and filtered them against its
current document snapshot inline. This slice makes the current behavior an
explicit `oran-skill` policy surface before adding expiration or deactivation.
The runtime behavior is intentionally unchanged: skill bodies remain
conversation-tail tool-result text, and section 4 changes only before the next
prompt when transcript metadata still matches a currently loaded and allowed
skill.

### Files Modified

- `include/oran/skill/catalog.hpp`
- `src/oran-skill/catalog.cpp`
- `src/oran-bootstrap/prompt_runner.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/skill/test_catalog.cpp`
- `docs/STATUS.md`
- `docs/ARCHITECTURE.md`
- `docs/design-docs/agent-platform.md`
- `docs/design-docs/bootstrap-runtime.md`
- `docs/product-specs/0009-skills.md`
- `docs/rules/prompt-design.md`
- `docs/QUALITY_SCORE.md`
- `docs/releases/feature-release-notes.md`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` - slice 143 snapshot, last-history pointer, validation, and
  next prompt-runtime direction.
- `docs/ARCHITECTURE.md` - `oran-skill`, `oran-prompt`, and `oran-agent`
  inventory rows now name the activation-policy owner.
- `docs/design-docs/agent-platform.md` - section-4 policy ownership.
- `docs/design-docs/bootstrap-runtime.md` - runner handoff to
  `skill::resolve_active_skills(...)`.
- `docs/product-specs/0009-skills.md` - explicit transcript-derived policy
  status and coverage note.
- `docs/rules/prompt-design.md` - prompt surface status note for slices 135-143.
- `docs/QUALITY_SCORE.md` - `test-skill` counts and skill policy state.
- `docs/releases/feature-release-notes.md` - user-visible slice 143 row.

### Validation

- Commands run:
  - `xmake run -y test-skill`
  - `timeout 60s xmake run -y test-bootstrap`
  - `git diff --check`
  - `xmake run orangutan -- --help`
  - `make ci`
- Tests added/changed: `tests/skill/test_catalog.cpp` now covers the policy
  resolver filtering transcript-derived active markers against available catalog
  entries, disabling transcript-derived markers, and rejecting duplicate
  available skill entries.
- Bench impact: no new bench; this is a small policy routing helper over the
  already-loaded skill catalog and transcript metadata, not a new hot-path
  algorithm.
- Compile-budget delta: no new target, dependency, or heavy public include.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md`
