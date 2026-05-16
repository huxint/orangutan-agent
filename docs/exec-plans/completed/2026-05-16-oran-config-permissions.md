# `oran-config` — `permissions` + `agents.<name>.permissions` Slice

## Goal

Land layer 2 and the data side of layer 3 of the three-layer rule
merge described in `docs/design-docs/permissions-and-hooks.md`
("Sources": built-in defaults → global config → per-agent overlay).
`oran-permission::Defaults::for_mode` already owns layer 1. This
slice teaches `oran-config` to parse the `permissions` root section
and each `agents.<name>.permissions` overlay into typed structs,
validating verdict keys and capability spellings at load time so
the future `oran-permission::materialize` consumer (next slice)
has typed data to merge instead of free-form JSON.

## Scope

- In scope:
  - New public types in `include/oran/config/config.hpp`:
    - `PermissionVerdict { allow, deny, ask }` enum mirroring
      `permission::Verdict` (separate enum to keep `oran-config`
      below `oran-permission` in the dependency direction).
    - `to_string_view(PermissionVerdict)` +
      `parse_permission_verdict(std::string_view)` helpers
      (parity with `core::Role` / `core::Capability`).
    - `PermissionRuleConfig { verdict, tool_pattern, capability }`
      where `capability` is `std::optional<core::Capability>` —
      validated at parse time, so the materializer maps directly.
    - `PermissionsConfig { rules: std::vector<PermissionRuleConfig> }`
      with declaration order preserved (allow → deny → ask within
      the JSON object's iteration order).
    - `AgentConfig { name, permissions }` exposing the per-agent
      overlay.
    - `Config::permissions()` and `Config::agents()` accessors
      returning `const PermissionsConfig&` and
      `std::span<const AgentConfig>` respectively.
  - Parser updates in `src/oran-config/config.cpp`:
    - `parse_permissions(json)` reads the `permissions` block,
      iterates `allow`/`deny`/`ask` arrays (each element an
      object `{tool_pattern, capability?}`), and returns a
      `PermissionsConfig` with rules appended in JSON-object
      iteration order so the operator's authoring intent
      survives.
    - `parse_agents(json)` reads `agents.<name>` and constructs
      `AgentConfig{name, permissions}`. Unknown agent fields warn
      (loose) or fail (strict), matching the root unknown-field
      policy — agent expansion (model overrides, prompts, hooks)
      lands on later slices.
    - Capability strings are resolved via
      `core::parse_capability`; unknown spellings always error.
    - Verdict-key strings are resolved via
      `parse_permission_verdict`; unknown verdict keys warn
      (loose) or fail (strict).
  - Tests in `tests/config/test_config.cpp` (or a new file —
    add the cases to the existing test file to keep the bucket
    a single TU):
    - Parse the canonical shape and assert per-verdict rule
      counts / tool patterns / capability values.
    - Capability resolves through `parse_capability`.
    - Per-agent overlay parses with a stable agent order.
    - Missing `tool_pattern` → `ErrorKind::config`.
    - Unknown capability spelling → `ErrorKind::config`.
    - Unknown verdict key → warns in loose mode, fails in
      strict.
    - Env substitution still flows through permission strings
      (`${VAR}` inside `tool_pattern`).
  - Bench `bench/config/scenarios/permissions.cpp`:
    - `config.parse_permissions_empty` — parse the example
      config (no permission rules) to anchor the cost of the
      surface lift.
    - `config.parse_permissions_typed` — parse a synthetic
      config with 16 mixed-scope permission rules and a single
      agent overlay so the typed-resolve path has signal.
  - Wire the new scenario into `bench/config/main.cpp` with a
    matching title block.
  - Update `config.example.json` to include a representative
    `permissions` block (the same as the design-doc example) and
    one example agent overlay so the file documents the new
    schema.
  - Docs:
    - `docs/ARCHITECTURE.md` — `oran-config` inventory row +
      slice-status note name `PermissionsConfig` /
      `AgentConfig` / `Config::permissions` /
      `Config::agents`.
    - `docs/QUALITY_SCORE.md` — Config row notes the new typed
      surface and bumps the test/bench counters.
    - `docs/design-docs/permissions-and-hooks.md` — Engine
      status block now points at layer-2 data parsed; only the
      runtime merge remains.
    - `docs/design-docs/secrets-and-state.md` — if it mentions
      config surface, add `permissions` / `agents` to the typed
      list (audit first).
    - `docs/releases/feature-release-notes.md` — new row.
    - History entry under `docs/histories/2026-05/`.
- Out of scope:
  - `materialize(defaults, global, per_agent) -> RuleSet` — that
    is slice 2, lives in `oran-permission`.
  - Approval / re2 input regex / audit logging (still on the
    product-spec follow-up list).
  - Other `agents.<name>.*` fields (model, prompts, hook
    overrides) — future slices.
  - Channel-overlay permissions — v1.1 per the product spec.

## Context

- Relevant docs:
  - `docs/design-docs/permissions-and-hooks.md` (Sources lists
    the three layers; the YAML shape in "Rule Shape" is what
    `permissions` mirrors in JSON).
  - `docs/product-specs/0008-permissions.md` (criterion 4 wants
    config-driven loading; this slice gives the config side
    types, the next slice gives the merge).
  - `docs/rules/critical-rules.md` (C6 — no heavy includes in
    public headers; C7 — explicit ctors; C17 — modern stdlib).
  - `docs/design-docs/module-boundaries.md` (config sits below
    permission in the layering; verdict and capability stay as
    separate enums per layer).
- Relevant code paths:
  - `include/oran/config/config.hpp` and `src/oran-config/config.cpp`.
  - `include/oran/core/capability.hpp` (resolved at parse).
  - `tests/config/test_config.cpp`, `bench/config/scenarios/`.
  - `config.example.json`.
- Constraints:
  - `oran-config` keeps `nlohmann::json` confined to its TU; the
    new public types are stdlib-only.
  - Adding `core::Capability` to the public header is fine — it
    is stdlib-only itself.
  - Parser stays expected-only; no exceptions cross the boundary.
- Compile-budget impact (if any):
  - One new enum + three new structs + two new parser branches.
    Implementation TU stays well under the `oran-config` ≤ 2.0 s
    budget (current build is ~1.4 s; this should add ≤ 100 ms).

## Risks

- Risk: capability strings drift if `core::Capability` adds a new
  enumerator without updating the example config or test
  fixtures. Mitigation: tests assert round-trip for at least one
  representative of every capability category (file/network/
  process/memory) so a missing spelling fails fast.
- Risk: agents block becomes a junk drawer once other fields
  land. Mitigation: keep `AgentConfig` minimal and document the
  expected future fields in the public header's comment so the
  next agent has a clear extension point.
- Risk: the verdict-keyed JSON shape (`allow`/`deny`/`ask` arrays)
  ties config schema to the v1 verdict universe; adding a fourth
  verdict in the future would force a new key. Mitigation: that
  was already the design-doc shape; adding verdicts is rare and
  would warrant a config schema migration anyway.

## Milestones

1. Land plan; settle the JSON shape (verdict-keyed arrays of
   `{tool_pattern, capability?}` rule objects).
2. Implement types + parser.
3. Update `config.example.json` to document the new shape.
4. Add tests.
5. Add bench A/B + wire into bench main.
6. Update docs / history / release notes.
7. Run validation and move plan to `completed/`.

## Validation

- Commands:
  - `xmake build oran-config`
  - `xmake build test-config && xmake run test-config`
  - `xmake build bench-config && xmake run bench-config`
  - `xmake build orangutan`
  - `xmake test`
  - `git diff --check`
  - `make ci`
- Manual checks:
  - Loading the updated `config.example.json` succeeds and the
    parsed `permissions().rules` matches the example's count.
- Bench comparison (if perf-relevant):
  - Empty vs. populated `permissions` block parse cost: the
    populated path is dominated by `parse_capability` and
    string copies; should stay sub-millisecond for a 16-rule
    fixture.

## Progress Log

- [x] Confirm scope.
- [x] Implement types + parser.
- [x] Update example config.
- [x] Add tests.
- [x] Add bench scenario.
- [x] Update docs.
- [x] Run validation.
- [x] Write history entry.
- [x] Add release note.
- [x] Move plan to `completed/`.

## Decision Log

- 2026-05-16: parse-time capability validation. Rationale: the
  materializer in the next slice should be a pure type-mapping
  step; deferring validation pushes config errors into runtime,
  where they are harder to attribute. Capability strings are
  config-authored, finite, and small enough to validate eagerly.
- 2026-05-16: `PermissionVerdict` is a separate enum from
  `permission::Verdict`. Rationale: `oran-config` sits below
  `oran-permission` in the layering; importing `Verdict` from
  permission would reverse it. The values are identical but
  the materializer maps explicitly so a future divergence (e.g.
  a `defer` verdict that only exists at config time) does not
  bleed across.
- 2026-05-16: agent overlays warn on unknown fields per
  `strict_config`. Rationale: matches the root behavior, gives
  operators a typo signal, and leaves a paper trail for future
  field expansion.

## Linked Artifacts

- Related design doc:
  `docs/design-docs/permissions-and-hooks.md` (Sources,
  Rule Shape).
- Related product spec: `docs/product-specs/0008-permissions.md`
  (criterion 4).
- PRs: local change.
- History entry: `docs/histories/2026-05/<timestamp>-oran-config-permissions.md`.
- Release note: `docs/releases/feature-release-notes.md`.
