## [2026-06-02 02:53] | Task: Skill Expiration Policy

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: none - this is the next narrow implementation slice after the
  explicit deactivation policy input.

### User Query

Continue the prompt-runtime arc with documentation-grounded implementation,
keeping each shipped step aligned with the current architecture and progress.

### Changes Overview

- Areas: `oran-skill`, prompt-runtime docs, release/status tracking.
- Key actions: extend `skill::ActivationPolicy` with optional
  `evaluation_time` plus explicit `SkillExpiration` rows; validate expiration
  names as unique single-line skill names; require caller-provided evaluation
  time whenever expirations are present; have
  `skill::resolve_active_skills(...)` subtract expired names from
  transcript-derived active markers after loaded/allowed catalog filtering; and
  bump the binary slice tag to `2.0.0-slice145`.

### Design Intent

Expiration belongs to section-4 prompt-boundary policy, but it must not read a
hidden wall clock in the renderer. This slice therefore exposes expiration as
explicit caller state and makes the evaluation instant explicit too. The default
configured-route runner still supplies no expiration rows, so operator behavior
does not change until a later runtime owner provides durable expiration events
or caller time.

### Files Modified

- `include/oran/skill/catalog.hpp`
- `src/oran-skill/catalog.cpp`
- `tests/skill/test_catalog.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `docs/STATUS.md`
- `docs/ARCHITECTURE.md`
- `docs/design-docs/agent-platform.md`
- `docs/design-docs/api-portability.md`
- `docs/design-docs/bootstrap-runtime.md`
- `docs/product-specs/0009-skills.md`
- `docs/product-specs/0016-prompt-and-tool-catalog-cache.md`
- `docs/rules/prompt-design.md`
- `docs/QUALITY_SCORE.md`
- `docs/releases/feature-release-notes.md`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` - slice 145 snapshot, last-history pointer, validation,
  and next prompt-runtime direction.
- `docs/ARCHITECTURE.md` - `oran-skill` and `oran-prompt` inventory rows now
  name explicit expiration policy inputs.
- `docs/design-docs/agent-platform.md` - section-4 policy ownership now covers
  caller-provided expiration rows and evaluation time.
- `docs/design-docs/api-portability.md` - cache invalidation note now ties
  expiration inputs to section-4 content hashes.
- `docs/design-docs/bootstrap-runtime.md` - runner docs clarify the default
  empty expiration policy.
- `docs/product-specs/0009-skills.md` - shipped slice-145 policy surface and
  coverage note.
- `docs/product-specs/0016-prompt-and-tool-catalog-cache.md` - shipped
  expiration policy status in the cache-boundary spec.
- `docs/rules/prompt-design.md` - slices 135-145 prompt-surface summary.
- `docs/QUALITY_SCORE.md` - `test-skill` counts and remaining skill-policy
  next steps.
- `docs/releases/feature-release-notes.md` - user-visible slice 145 row.

### Validation

- Commands run:
  - `xmake run -y test-skill`
- Tests added/changed: `tests/skill/test_catalog.cpp` covers explicit
  expiration subtraction, deterministic repeated resolution, missing evaluation
  time rejection, duplicate expiration rejection, and blank expiration
  rejection.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-06`
