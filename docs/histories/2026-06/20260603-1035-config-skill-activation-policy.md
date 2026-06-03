## [2026-06-03 10:35] | Task: Config-Sourced Skill Activation Policy

### Execution Context

- Agent: `Claude`
- Base model: `Opus 4.8 (1M context)`
- Runtime: `Claude Code`
- Linked plan: none — this is the next narrow implementation slice named by
  `docs/STATUS.md` after slice 145 (a runtime-owned source of skill
  deactivation/expiration policy inputs).

### User Query

> Deeply understand the project architecture and current progress, then —
> after confirming the config-driven direction — implement the next slice
> end-to-end. The runtime-owned source for `skill::ActivationPolicy` should be
> per-agent config, keeping the section-4 renderer clock-free.

### Changes Overview

- Areas: `oran-config`, bootstrap prompt runner, `config.example.json`, tests,
  prompt-runtime docs, release/status tracking.
- Key actions: add typed `agents.<name>.skills_deactivated` and
  `agents.<name>.skills_expirations` config inputs; have `AgentPromptRunner`
  build a non-empty `skill::ActivationPolicy` from the selected agent config,
  mapping `config::SkillExpirationConfig` to `skill::SkillExpiration` and
  supplying `evaluation_time = core::time::now_utc()` at the prompt boundary
  only when expirations are present; and bump the binary slice tag to
  `2.0.0-slice146`.

### Design Intent

Slices 143-145 made `skill::ActivationPolicy` carry explicit deactivation names,
expiration rows, and a caller-supplied `evaluation_time`, but the bootstrap
runner still passed a default `skill::ActivationPolicy{}`, so the capability was
inert. This slice supplies the first runtime-owned source for those inputs.

Per-agent config was chosen (over a tool/hook event or a durable session store)
because it mirrors the existing `agents.<name>.skills_enabled` /
`prompt_overlay` precedent, is the smallest increment that closes the inert-policy
gap, and keeps the layering clean: `oran-config` (platform layer) cannot depend
on `oran-skill`, so config owns its own `SkillExpirationConfig` value type and
bootstrap (which sees both) maps it to `skill::SkillExpiration`. The runner — not
the renderer — reads the wall clock for `evaluation_time`, so the section-4
renderer stays clock-free and `prompt-design.md`'s cache discipline holds:
identical loaded/allowed snapshots plus identical policy inputs render
byte-identical section-4 text, while a crossed expiry or a deactivation changes
section 4 only at the next prompt boundary.

### Files Modified

- `include/oran/config/config.hpp`
- `src/oran-config/config.cpp`
- `src/oran-bootstrap/prompt_runner.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `config.example.json`
- `tests/config/test_config.cpp`
- `tests/bootstrap/test_prompt_runner.cpp`
- `docs/STATUS.md`
- `docs/ARCHITECTURE.md`
- `docs/design-docs/agent-platform.md`
- `docs/design-docs/bootstrap-runtime.md`
- `docs/product-specs/0009-skills.md`
- `docs/rules/prompt-design.md`
- `docs/QUALITY_SCORE.md`
- `docs/releases/feature-release-notes.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 146 snapshot, last-history pointer, validation,
  refreshed `oran-config` / `oran-bootstrap` surface counts, and next
  prompt-runtime direction.
- `docs/ARCHITECTURE.md` — `oran-config` row names the new agent skill
  activation-policy inputs; `oran-bootstrap` row notes the runner now builds a
  non-empty `skill::ActivationPolicy` from selected-agent config.
- `docs/design-docs/agent-platform.md` — section-4 policy ownership now names
  config as the deactivation/expiration source.
- `docs/design-docs/bootstrap-runtime.md` — runner now reads
  `agents.<name>.skills_deactivated` / `skills_expirations` and supplies the
  prompt-boundary evaluation time.
- `docs/product-specs/0009-skills.md` — shipped slice-146 config source and the
  remaining v1.1 downstream (durable/event-driven) note.
- `docs/rules/prompt-design.md` — slices 135-146 prompt-surface summary.
- `docs/QUALITY_SCORE.md` — `test-config` / `test-bootstrap` counts and the
  skill-policy state.
- `docs/releases/feature-release-notes.md` — user-visible slice 146 row.

### Validation

- Commands run:
  - `xmake build test-config && build/.../test-config` — 39 cases / 299 assertions.
  - `xmake build test-bootstrap && build/.../test-bootstrap` — 98 cases / 646
    assertions (includes a config-deactivation case, a config-expiration
    past-suppresses case, and a not-yet-expired preserves case; the latter uses
    a `system_clock`-representable future year).
  - `xmake build test-skill && build/.../test-skill` — 21 cases / 133 assertions,
    unchanged (`oran-skill` not modified).
  - `xmake build orangutan` + `xmake run orangutan -- --help` — banner reports
    `2.0.0-slice146`.
  - `make ci`.
- Tests added/changed: `tests/config/test_config.cpp` covers parsing the two new
  agent fields plus malformed deactivation / expiration timestamp / missing name
  / non-array failures; `tests/bootstrap/test_prompt_runner.cpp` covers the
  runner applying config deactivation and expiration to section-4 active markers.
- Bench impact: none; the runner builds a small policy struct over already-loaded
  config and resolves it with the existing `skill::resolve_active_skills`
  helper — no new hot-path algorithm.
- Compile-budget delta: no new target or dependency; `config.hpp` adds the
  light `<oran/core/time.hpp>` (already used by several public headers).

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md#2026-06`
