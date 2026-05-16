# `oran-permission` — `Defaults` Baseline Factory Slice

## Goal

Land the third slice of `oran-permission`: a `Defaults::for_mode(Mode)`
factory that returns the safe-baseline `RuleSet` the design doc has
been promising. `docs/design-docs/permissions-and-hooks.md` says
"Rules come from three layers, merged at runtime: 1. Built-in defaults
(`oran-permission::Defaults`) — safe baseline; 2. Global config; 3.
Per-agent overlay." The foundation slice landed without layer 1; this
slice adds it. The actual `merge(defaults, config, agent_overlay)`
helper that combines the layers belongs on a future slice once
`oran-config` knows how to materialize a `RuleSet` — without that
consumer the merge would be code-without-a-caller.

## Scope

- In scope:
  - New public header `include/oran/permission/defaults.hpp` and TU
    `src/oran-permission/defaults.cpp`.
  - `permission::Defaults` (struct, single static method) exposing
    `Defaults::for_mode(Mode mode) -> RuleSet`. The returned rule
    set is documented per-mode; tests assert the shape of every
    mode's output.
  - Mode-by-mode baselines (encoded as `capability=`-scoped rules
    so they survive tool renames per the design doc):
    - `Mode::strict` → empty baseline; the operator must
      explicitly allow what they need.
    - `Mode::default_` → allow `read_file` + `read_memory`; ask
      `write_file`, `edit_file`, `write_memory`,
      `spawn_subprocess`, `egress_http`; deny `runtime_loader`
      and `delete_path`.
    - `Mode::permissive` → deny `runtime_loader` and `delete_path`
      only; the mode's default verdict handles the rest.
    - `Mode::sandboxed` → allow `read_file` and `read_memory`
      only; the mode's default verdict denies everything else.
  - Tests: `tests/permission/test_defaults.cpp` — every mode
    returns the expected shape (rule count + verdict per
    capability spelling), `Defaults::for_mode` is referentially
    transparent (two calls produce equal `RuleSet`s by `size()`
    and rule-by-rule equality where exposed), and the baseline
    correctly classifies a representative call for each mode
    (e.g. `Mode::default_` allows a `read_file`-required call,
    asks a `write_file`-required call, denies a
    `runtime_loader`-required call, falls back to ask on a
    capability-less call).
  - Bench: `bench/permission/scenarios/defaults.cpp` — A/B between
    `permission.defaults_build_default` (the factory) and
    `permission.defaults_hand_built_default` (the same rules
    built inline). Documents the cost of the factory vs. the
    bare construction; consumed by future config loaders that
    will pay the cost once at startup.
  - Wire the new bench scenario into `bench/permission/main.cpp`.
  - Update the umbrella header `include/oran/permission.hpp` to
    expose `defaults.hpp` so `<oran/permission.hpp>` keeps being
    the only sanctioned consumer entry point.
  - Docs: `docs/ARCHITECTURE.md` `oran-permission` row (mentions
    `Defaults`), `docs/QUALITY_SCORE.md` permission row + test /
    bench counters, `docs/design-docs/permissions-and-hooks.md`
    engine-status note + the "Sources" subsection (note layer 1
    is live), release notes, history entry.
- Out of scope:
  - The merge helper combining defaults + config + per-agent
    overlay — belongs with the config-wiring slice that owns
    layers 2 and 3.
  - Config-driven baseline overrides.
  - HMAC approvals, audit logging, re2 input regex.
  - Tweaking the mode-by-mode capability lists based on
    operational experience — the baseline ships an opinion; tuning
    is a separate slice once we have feedback.

## Context

- Relevant docs:
  - `docs/design-docs/permissions-and-hooks.md` ("Sources" lists
    the three layers; "Modes" lists the per-mode default
    behavior; "Capability-Aware Gating" pins the
    `capability=`-scoped rule shape).
  - `docs/product-specs/0008-permissions.md` (the v1 deliverables
    include the rule modes and capability gating; defaults are
    part of the same shipped surface).
  - `docs/rules/critical-rules.md` (C6, C17 — no heavy includes
    in public headers; modern stdlib; ranges over loops).
- Relevant code paths:
  - `include/oran/permission/rule_set.hpp` (consumed by the new
    factory).
  - `include/oran/permission.hpp` (umbrella).
  - `src/oran-permission/`, `tests/permission/`,
    `bench/permission/`.
- Constraints:
  - `oran-permission` continues to depend only on `oran-core`.
  - The new public header pulls in stdlib only via
    `rule_set.hpp`. No `<algorithm>` / `<ranges>` exposure.
  - `Defaults::for_mode` is `noexcept(false)` (allocates the
    returned `RuleSet`) but never throws under normal use; if
    `RuleSet::add` ever started to throw, the function would
    propagate.
- Compile-budget impact (if any):
  - One small public header + one TU. Stays well under the
    `oran-permission` ≤ 2.5 s per-TU budget category.

## Risks

- Risk: the per-mode baseline encodes opinions about which
  capabilities are "safe" / "ask-worthy" / "dangerous", and an
  operator might disagree. Mitigation: defaults are intended to
  be layered under config-driven rules; the operator can deny a
  default `allow` with an explicit `deny`. The history entry
  documents the rationale for each capability's verdict per mode
  so future tuning has a paper trail.
- Risk: the slice grows in scope as soon as someone wants
  `Defaults::for_profile(Profile)` or per-channel defaults.
  Mitigation: keep the surface to `for_mode(Mode)` only; profiles
  and channel overlays land alongside config wiring.
- Risk: adding new modes later forces a baseline update. Mitigation:
  the `for_mode` implementation switches on `Mode`; a missing case
  would fall through to an empty `RuleSet` (safe — the mode default
  still applies). Tests verify every existing enumerator yields a
  documented baseline.

## Milestones

1. Land plan, settle per-mode baselines.
2. Implement `Defaults::for_mode` + register in umbrella.
3. Write tests.
4. Write bench A/B + wire into bench main.
5. Update docs / history / release notes.
6. Run validation and move plan to `completed/`.

## Validation

- Commands:
  - `xmake build oran-permission`
  - `xmake build test-permission && xmake run test-permission`
  - `xmake build bench-permission && xmake run bench-permission`
  - `xmake test`
  - `xmake build orangutan`
  - `git diff --check`
  - `make ci`
- Manual checks:
  - Public header includes only `rule_set.hpp` (which already
    pulled the stdlib needed).
  - Each `Defaults::for_mode(mode)` returns the documented rule
    count and verdicts per capability.
- Bench comparison (if perf-relevant):
  - `permission.defaults_build_default` (factory) vs.
    `permission.defaults_hand_built_default` (inline) over the
    `Mode::default_` baseline so future config-loading paths can
    see the cost of the factory at startup.

## Progress Log

- [x] Confirm scope.
- [x] Implement `Defaults::for_mode` + umbrella update.
- [x] Add tests.
- [x] Add bench scenario.
- [x] Update docs.
- [x] Run validation.
- [x] Write history entry.
- [x] Add release note.
- [x] Move plan to `completed/`.

## Decision Log

- 2026-05-16: keep the slice surface to `Defaults::for_mode(Mode)`
  only. Rationale: profiles / per-channel overlays / per-agent
  baselines all want the same mechanism but with extra inputs,
  and shipping those without a real config-driven caller would
  bake API decisions we can't yet make.
- 2026-05-16: `Mode::strict` returns an *empty* baseline.
  Rationale: strict mode's defining property is "deny by default,
  allow only explicit"; a strict mode that ships pre-allowed
  capabilities contradicts itself. Operators who want a strict
  baseline with read-side allowed pick `Mode::sandboxed` instead.

## Linked Artifacts

- Related design doc:
  `docs/design-docs/permissions-and-hooks.md` (Sources, Modes,
  Capability-Aware Gating).
- Related product spec: `docs/product-specs/0008-permissions.md`.
- PRs: local change.
- History entry: `docs/histories/2026-05/<timestamp>-oran-permission-defaults.md`
- Release note: `docs/releases/feature-release-notes.md`
