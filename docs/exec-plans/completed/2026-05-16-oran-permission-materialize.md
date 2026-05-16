# `oran-permission` — Three-Layer `materialize` Slice

## Goal

Land the final piece of the design-doc three-layer rule merge.
`Defaults::for_mode(Mode)` (layer 1) shipped previously;
`Config::permissions()` + `Config::agents()` (layer 2 and the
data side of layer 3) shipped in the preceding
`oran-config-permissions` slice. This slice adds
`oran-permission::materialize` — the function that concatenates
all three layers into a single `RuleSet` ready to feed the
runtime evaluator. With this slice the three-layer merge is
end-to-end live; no consumer code is missing.

## Scope

- In scope:
  - New public header
    `include/oran/permission/materialize.hpp` and TU
    `src/oran-permission/materialize.cpp`.
  - `permission::materialize(Mode, const config::PermissionsConfig& global, const config::PermissionsConfig& per_agent) -> RuleSet`.
  - Convenience overload
    `permission::materialize(Mode, const config::PermissionsConfig& global) -> RuleSet`
    for call sites that don't have a per-agent overlay yet (and
    for terser tests). It passes an empty
    `PermissionsConfig` for the per-agent layer.
  - Internal helper
    `to_verdict(config::PermissionVerdict) noexcept -> Verdict`
    (TU-local) that maps the config-side enum to the runtime
    `permission::Verdict`. One-to-one switch; documents the
    fact that the two enums are intentionally separate.
  - Layer order: `Defaults::for_mode(mode)` rules go in first,
    then `global.rules` in source order, then `per_agent.rules`
    in source order. The runtime evaluator's deny → allow → ask
    precedence is unchanged; layering matters only for the
    "first match wins" tie-breaker within a verdict.
  - Wire `oran-permission` to depend on `oran-config` in
    `xmake/targets.lua`. The header pulls in
    `<oran/config/config.hpp>` for the `PermissionsConfig`
    type; the TU includes both that and
    `<oran/permission/defaults.hpp>`. `oran-config` was already
    below `oran-permission` in the layering, so this is a
    legal dep add.
  - Umbrella `include/oran/permission.hpp` re-exports
    `materialize.hpp`.
  - Tests in a new file `tests/permission/test_materialize.cpp`:
    - Materialize with empty config + empty overlay equals
      `Defaults::for_mode(mode)`.
    - Materialize with one global rule appends after defaults.
    - Per-agent overlay rules append after global rules.
    - Materialize honors capability scope on config-side rules
      (a config `deny: {capability: runtime_loader}` still
      fires for a `runtime_loader` call).
    - Materialize honors verdict mapping
      (config `ask` → runtime `Verdict::ask`).
    - Deny in *any* layer outranks allow in any other layer
      (since the runtime evaluator walks all denies before any
      allow), demonstrating the layer concatenation correctly
      feeds the existing precedence walk.
    - Order within a layer is preserved.
  - Bench `bench/permission/scenarios/materialize.cpp`:
    - `permission.materialize_defaults_only` — `materialize`
      with empty config (closest equivalent of
      `Defaults::for_mode`).
    - `permission.materialize_with_global` — populated 8-rule
      global only.
    - `permission.materialize_with_global_and_agent` — full
      three-layer merge with the same global + a 2-rule
      overlay.
  - Wire the new bench scenario into `bench/permission/main.cpp`.
  - Docs:
    - `docs/ARCHITECTURE.md` — `oran-permission` inventory row
      + slice-status note name the new `materialize` API and
      the now-live three-layer merge.
    - `docs/QUALITY_SCORE.md` — Permissions row notes the new
      materializer, bumps the test / bench counters, and
      updates the "Next Step" to refer to re2 / HMAC / audit.
    - `docs/design-docs/permissions-and-hooks.md` — Engine
      status now says the runtime merge is live.
    - `docs/product-specs/0008-permissions.md` — acceptance
      criterion #4 annotated "Foundation landed 2026-05-16"
      (config loading is now end-to-end; re2 patterns remain).
    - `docs/releases/feature-release-notes.md` — new row.
    - History entry under `docs/histories/2026-05/`.
- Out of scope:
  - re2 / `InputPattern` runtime regex.
  - HMAC-signed approval prompts + replay TTL.
  - Audit logging.
  - A higher-level "build per-agent rule set by agent name"
    helper — that wants an agent-resolution surface that the
    `oran-bootstrap` runtime assembly slice owns, not us.

## Context

- Relevant docs:
  - `docs/design-docs/permissions-and-hooks.md` (Sources +
    Evaluation describe the merge and precedence).
  - `docs/design-docs/module-boundaries.md` (`oran-permission`
    sits above `oran-config`; this slice formalizes the
    dependency add).
  - `docs/product-specs/0008-permissions.md` (criterion 4 wants
    config-driven loading wired through; this slice completes
    that for the foundation surface).
- Relevant code paths:
  - `include/oran/permission/rule_set.hpp` (consumed).
  - `include/oran/permission/defaults.hpp` (consumed).
  - `include/oran/config/config.hpp` (consumed).
  - `src/oran-permission/`, `tests/permission/`,
    `bench/permission/`.
- Constraints:
  - The new public header pulls in `<oran/config/config.hpp>`,
    which is stdlib-only on the public side, so the
    `oran-permission` public surface stays third-party-free.
  - The TU is small: one switch + one loop per layer. Stays
    well under the `oran-permission` ≤ 2.5 s per-TU budget.
- Compile-budget impact (if any):
  - One small public header + one TU. The
    `<oran/config/config.hpp>` include touches
    `<oran/core/capability.hpp>` and a few stdlib headers; all
    are already pulled in by other `oran-permission`
    translation units.

## Risks

- Risk: layer ordering ambiguity. The design doc says "later
  layers override earlier ones; explicit deny always wins over
  allow." Concatenating layers in defaults → global → per-agent
  order and reusing the existing deny → allow → ask precedence
  walk produces exactly that: a later-layer `allow` of a tool
  that an earlier-layer `deny` already covered still loses
  (deny pass runs first). Mitigation: tests assert the
  cross-layer precedence explicitly.
- Risk: someone reads "later overrides earlier" as "later
  replaces earlier" and expects a per-rule diff/merge.
  Mitigation: the design doc and the new history make clear
  that "override" means "the deny → allow → ask precedence
  walk visits later layers' rules too", not "later layers
  silently drop earlier layers' rules". The cleaner semantic
  belongs upstream once the design doc grows the case for it.
- Risk: dependency direction. Adding `oran-config` as a dep of
  `oran-permission` is layer-legal (permission sits above
  config) but the public header now pulls in
  `<oran/config/config.hpp>`. Mitigation: the config public
  header is stdlib-only; no transitive heavy includes leak in.

## Milestones

1. Land plan; settle layer ordering + verdict mapping.
2. Implement materializer + umbrella update.
3. Wire `oran-config` into the `oran-permission` target.
4. Write tests.
5. Write bench A/B + wire into bench main.
6. Update docs / history / release notes.
7. Run validation and move plan to `completed/`.

## Validation

- Commands:
  - `xmake build oran-permission`
  - `xmake build test-permission && xmake run test-permission`
  - `xmake build bench-permission && xmake run bench-permission`
  - `xmake build orangutan`
  - `xmake test`
  - `git diff --check`
  - `make ci`
- Manual checks:
  - Public header pulls in only `<oran/config/config.hpp>` +
    `<oran/permission/rule_set.hpp>`; no other heavy includes.
  - Materialized rule sets have `defaults.size() +
    global.rules.size() + per_agent.rules.size()` entries.
- Bench comparison (if perf-relevant):
  - `materialize_defaults_only` vs.
    `materialize_with_global_and_agent` documents how the
    config-side rules add to startup cost.

## Progress Log

- [x] Confirm scope.
- [x] Implement materializer + umbrella update.
- [x] Add `oran-config` dep to `oran-permission`.
- [x] Add tests.
- [x] Add bench scenario.
- [x] Update docs.
- [x] Run validation.
- [x] Write history entry.
- [x] Add release note.
- [x] Move plan to `completed/`.

## Decision Log

- 2026-05-16: layer concatenation, not per-rule diff/merge.
  Rationale: the design doc's evaluator runs deny → allow →
  ask in three passes; concatenation lets later layers add
  rules that the evaluator visits naturally. A per-rule
  override mechanism (e.g. by `(verdict, tool_pattern,
  capability)` triple) would silently drop rules and surprise
  operators reading the audit log.
- 2026-05-16: ship a two-argument convenience overload. Rationale:
  most call sites today are tests or a single-agent runtime;
  forcing them to type an empty `PermissionsConfig{}` for the
  per-agent layer is friction without benefit.

## Linked Artifacts

- Related design doc:
  `docs/design-docs/permissions-and-hooks.md` (Sources,
  Evaluation).
- Related product spec: `docs/product-specs/0008-permissions.md`
  (criterion 4).
- PRs: local change.
- History entry: `docs/histories/2026-05/<timestamp>-oran-permission-materialize.md`.
- Release note: `docs/releases/feature-release-notes.md`.
