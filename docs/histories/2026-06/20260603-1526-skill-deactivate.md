## [2026-06-03 15:26] | Task: skill.deactivate Built-in (Transcript-Event Activation Source)

### Execution Context

- Agent: `Claude`
- Base model: `Opus 4.8 (1M context)`
- Runtime: `Claude Code`
- Linked plan: none — this is the next narrow implementation slice named by
  `docs/STATUS.md` after slice 146 (a durable or event-driven source for
  `skill::ActivationPolicy` inputs so activation state can change mid-session
  without a config edit).

### User Query

> Deeply understand the project architecture and current progress, read the
> relevant docs, merge the completed slice to `main`, then continue with the
> next implementation. The chosen direction for slice 147 is a permissioned
> `skill.deactivate` built-in (a transcript-event source), keeping the
> section-4 renderer clock-free; do not push to origin.

### Changes Overview

- Areas: `oran-core` (capability), `oran-skill` (record + transcript scan),
  `oran-tool` (built-in + dispatch-context callback), `oran-prompt` /
  `oran-agent` (default active tools), `oran-agent` scheduler (per-call context
  threading), bootstrap prompt runner (callback wiring), plus tests and docs.
- Key actions: add `core::Capability::deactivate_skill`; add
  `skill::render_deactivation_data_json` / `deactivated_skill_from_data_json`
  and make `skill::active_skills_from_transcript` order-aware (invoke adds,
  deactivate removes, most recent transcript event wins); add the
  `skill.deactivate` built-in delegating through a new
  `DispatchContext::skill_deactivate` callback; thread that callback through
  `ToolScheduler` per-call contexts; install the bootstrap runner callback that
  returns a versioned `skill_deactivation` record; and bump the binary slice tag
  to `2.0.0-slice147`.

### Design Intent

Slices 142-146 made section-4 active markers transcript-derived (`skill.invoke`)
plus explicit config-sourced deactivation/expiration. The remaining v1.1 gap was
a way for the agent to drop an active skill *mid-session* without a config edit.

A transcript-event `skill.deactivate` built-in was chosen over a session-store
activation table because it mirrors the existing `skill.invoke` precedent
exactly: the same `data_json` record shape, the same `DispatchContext` callback
seam (so `oran-tool` stays independent of `oran-skill`), and the same transcript
that the session store already persists (slice 132) — so it is durable across
restarts with no new repository or migration. Deactivation is modeled as its own
`deactivate_skill` capability (not a reuse of `invoke_skill`) because it is a
distinct action an operator may want to gate independently; like `invoke_skill`
it carries no explicit `Defaults::for_mode` rule, so it inherits the same
per-mode catch-all and the permission posture stays symmetric. The netting lives
in `skill::active_skills_from_transcript` (transcript-event source), leaving the
slice-144 config `deactivated_skill_names` as a separate policy override — the
two sources compose. The renderer stays clock-free: deactivation is a discrete
transcript event, resolved only at the prompt boundary, so `prompt-design.md`'s
cache discipline holds (identical loaded/allowed snapshot + transcript +
policy → byte-identical section 4).

### Files Modified

- `include/oran/core/capability.hpp`
- `include/oran/skill/catalog.hpp`
- `src/oran-skill/catalog.cpp`
- `include/oran/tool/registry.hpp`
- `include/oran/tool/builtins.hpp`
- `src/oran-tool/skill_deactivate.cpp` (new)
- `src/oran-tool/builtins.cpp`
- `src/oran-prompt/builder.cpp`
- `src/oran-agent/loop.cpp`
- `src/oran-agent/scheduler.cpp`
- `src/oran-bootstrap/prompt_runner.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/core/test_capability.cpp`
- `tests/skill/test_catalog.cpp`
- `tests/tool/test_registry.cpp`
- `tests/bootstrap/test_prompt_runner.cpp`
- docs (see below)

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/tool-runtime.md` — `Capability` enum gains `deactivate_skill`
  (the `check-docs-sync.sh`-validated list), the `oran-tool-skill` built-in row
  now lists `skill.deactivate`, and a slice-147 status note describes the
  built-in plus its catch-all permission posture.
- `docs/product-specs/0009-skills.md` — new Scope(v1) `skill.deactivate` entry,
  the v1.1 downstream note marks the event-driven source shipped, and
  acceptance criterion 6 cites the new coverage.
- `docs/design-docs/agent-platform.md` — section-4 ownership now names the
  transcript-event deactivation source.
- `docs/design-docs/bootstrap-runtime.md` — the runner now installs a
  `DispatchContext::skill_deactivate` callback and the policy derivation nets
  invoke against deactivate transcript results.
- `docs/design-docs/permissions-and-hooks.md` — capability-aware gating note for
  the skill-management capabilities and their catch-all defaults.
- `docs/ARCHITECTURE.md` — `oran-core` capability count 20 → 21; `oran-skill`,
  `oran-tool`, and `oran-bootstrap` rows describe the deactivation record,
  built-in, and runner callback.
- `docs/rules/prompt-design.md` — slices 135-147 summary includes the
  `skill.deactivate` transcript-event source.
- `docs/QUALITY_SCORE.md` — refreshed `test-skill` / `test-tool` / `test-core` /
  `test-bootstrap` counts and the skill / bootstrap / tool / prompt rows.
- `docs/releases/feature-release-notes.md` — user-visible slice 147 row.
- `docs/STATUS.md` — slice 147 snapshot, last-history pointer, next intended
  slice, and refreshed library surface counts.

### Validation

- Commands run:
  - `xmake -j$(nproc)` — clean build of all libs, the `orangutan` binary, tests,
    and benches (build ok, ~56 s; only pre-existing `route_profile_used`
    field-init warnings in untouched `protocol_response.cpp`).
  - `xmake test` — **16/16 buckets pass**. Affected buckets: `test-core`
    71 cases / 459 assertions, `test-skill` 24 / 155, `test-tool` 191 / 1919,
    `test-prompt` 10 / 98, `test-agent` 56 / 10 744, `test-bootstrap` 99 / 667.
  - `make ci` — green.
- Tests added/changed: `tests/core/test_capability.cpp` (enumerator + count 21);
  `tests/skill/test_catalog.cpp` (deactivation round-trip / record distinctness,
  order-aware transcript netting incl. re-activation, `resolve_active_skills`
  through a transcript deactivation); `tests/tool/test_registry.cpp`
  (`register_skill_deactivate` advertise, catalog size 8 → 9, dispatch delegation,
  missing-runtime error); `tests/bootstrap/test_prompt_runner.cpp` (end-to-end
  invoke → active → deactivate → cleared-at-next-boundary, with the
  `skill_deactivation` record asserted).
- Bench impact: none; the built-in reuses the existing dispatch path and the
  transcript scan is not a new hot-path algorithm (no new bench scenario, mirroring
  slice 146).
- Compile-budget delta: no new target or dependency; one new small `oran-tool`
  TU (`skill_deactivate.cpp`) and one new `core::Capability` enumerator.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md#2026-06`.

### Note

The bootstrap end-to-end test caught a real defect during development:
`ToolScheduler::make_per_call_context` brace-initializes each per-call
`DispatchContext` field-by-field and did not copy the new `skill_deactivate`
callback, so the scheduled `skill.deactivate` dispatch hit the "runtime service
unavailable" path. Fixed by threading `skill_deactivate` alongside `skill_invoke`
in `src/oran-agent/scheduler.cpp`.
