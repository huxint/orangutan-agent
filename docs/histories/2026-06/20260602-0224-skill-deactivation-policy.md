## [2026-06-02 02:24] | Task: Skill Deactivation Policy

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: none - this was the first narrow implementation slice after the
  section-4 cache-semantics documentation slice.

### User Query

Continue the prompt-runtime arc with documentation-grounded implementation,
keeping each shipped step aligned with the current architecture and progress.

### Changes Overview

- Areas: `oran-skill`, prompt-runtime docs, release/status tracking.
- Key actions: extend `skill::ActivationPolicy` with
  `deactivated_skill_names`; validate explicit deactivation names as unique
  single-line skill names; have `skill::resolve_active_skills(...)` subtract
  those names from transcript-derived active markers after loaded/allowed
  catalog filtering; and bump the binary slice tag to `2.0.0-slice144`.

### Design Intent

The previous slice made section-4 cache semantics explicit before adding more
policy behavior. Explicit deactivation is the smallest implementation step that
exercises that contract: it is deterministic caller-provided policy input,
changes only the next prompt's section-4 active-marker set, and needs no hidden
clock or durable runtime state. Configured-route bootstrap still supplies an
empty deactivation set, so runtime behavior is unchanged until a later owner
provides real deactivation events.

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

- `docs/STATUS.md` - slice 144 snapshot, last-history pointer, validation,
  and next prompt-runtime direction.
- `docs/ARCHITECTURE.md` - `oran-skill` and `oran-prompt` inventory rows now
  name explicit deactivation policy input.
- `docs/design-docs/agent-platform.md` - section-4 policy ownership now covers
  caller-provided deactivation names.
- `docs/design-docs/api-portability.md` - cache invalidation note now ties
  `deactivated_skill_names` to section-4 content hashes.
- `docs/design-docs/bootstrap-runtime.md` - runner docs clarify the default
  empty deactivation set.
- `docs/product-specs/0009-skills.md` - shipped slice-144 policy surface and
  coverage note.
- `docs/product-specs/0016-prompt-and-tool-catalog-cache.md` - shipped
  deactivation policy status in the cache-boundary spec.
- `docs/rules/prompt-design.md` - slices 135-144 prompt-surface summary.
- `docs/QUALITY_SCORE.md` - `test-skill` counts and remaining skill-policy
  next steps.
- `docs/releases/feature-release-notes.md` - user-visible slice 144 row.

### Validation

- Commands run:
  - `xmake run -y test-skill`
  - `timeout 60s xmake run -y test-bootstrap`
- Tests added/changed: `tests/skill/test_catalog.cpp` covers explicit
  deactivation subtraction, deterministic repeated resolution, duplicate
  deactivation rejection, and blank deactivation rejection.
- Bench impact: no new bench; this is a small deterministic set subtraction on
  already materialized prompt-boundary policy inputs.
- Compile-budget delta: no new target, dependency, or heavy public include.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md`
