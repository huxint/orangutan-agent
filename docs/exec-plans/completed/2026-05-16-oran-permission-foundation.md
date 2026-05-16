# `oran-permission` — Foundation Slice (`Verdict` + `RuleSet`)

## Goal

Bring up the `oran-permission` library at its smallest useful surface so the
quality-score row for permissions moves from `D` (engine designed, no
implementation) to `C` (foundation lands, smoke-tested). The slice ships the
non-capability, non-regex, non-audit core: a `Verdict` enum, a tool-name glob
`Rule`, a `RuleSet` evaluator with the design-doc precedence (deny first,
then allow, then ask, then default-by-mode), and a `Mode` enum that picks the
fallback verdict. No `re2`, no HMAC approvals, no audit logging yet — those
land in later slices per `docs/product-specs/0008-permissions.md`.

## Scope

- In scope:
  - New library target `oran-permission`, public header
    `include/oran/permission/rule_set.hpp` (with a thin umbrella
    `include/oran/permission.hpp`), implementation TU
    `src/oran-permission/rule_set.cpp`.
  - Types:
    - `permission::Verdict { allow, deny, ask }` with
      `to_string_view(Verdict)`.
    - `permission::Mode { strict, default_, permissive, sandboxed }` with
      `to_string_view(Mode)`.
    - `permission::Rule { Verdict verdict; std::string tool_pattern; }`
      with member-wise equality.
    - `permission::Decision { Verdict verdict; std::string reason; }`
      describing the rule index or default mode that produced the verdict.
    - `permission::RuleSet` with `add(Rule)`, `clear()`, `size()`, and
      `evaluate(std::string_view tool_name, Mode mode) const -> Decision`.
  - Tool-name pattern matching: simple glob with `*` matching any
    sequence of characters (including empty). No `?`, no character
    classes. Implemented as a textbook recursive matcher; the future
    re2 path will live in a separate type.
  - Tests: `tests/permission/test_rule_set.cpp` covering literal match,
    `*` wildcard, deny-wins precedence, allow-first match, ask
    precedence, default-by-mode for every mode, and empty rule set.
  - Bench: `bench/permission/scenarios/rule_set.cpp` comparing
    `permission::RuleSet::evaluate` against a hand-rolled linear scan
    (`std::ranges::find_if` on a flat `std::vector<Rule>` with the same
    precedence). Documents the cost of the precedence-respecting walk
    over a 16-rule fixture vs. the cheapest possible filter.
  - Library scaffolding: `xmake/targets.lua`, `xmake/tests.lua`,
    `xmake/bench.lua` updates; `orangutan` binary target picks up the
    new lib so its symbols ship in the binary that already exists.
  - Docs: `docs/ARCHITECTURE.md` slice-status + inventory,
    `docs/QUALITY_SCORE.md` (move permissions to `C`, update test /
    bench rows), `docs/design-docs/permissions-and-hooks.md` (note the
    foundation slice and what is still deferred), `bench/README.md`,
    `bench/permission/README.md`, `tests/permission/README.md` if
    needed, release notes, and a history entry.
- Out of scope:
  - `re2` regex on `InputPattern`.
  - Capability gating.
  - HMAC-signed approval prompts and replay TTL.
  - Audit log writes to `audit.db`.
  - Config loader wiring (`oran-config::permissions`).
  - Per-channel overlays, time-bound approvals, sticky approvals.
  - Permission `Error` enrichment beyond the existing
    `core::ErrorKind::permission_denied` value (no change required for
    this slice).

## Context

- Relevant docs:
  - `docs/product-specs/0008-permissions.md` — the v1 surface; this
    slice implements a strict subset.
  - `docs/design-docs/permissions-and-hooks.md` — the rule-shape and
    precedence algorithm we follow verbatim.
  - `docs/ARCHITECTURE.md` — `oran-permission` is allowed to depend
    only on `oran-core`.
- Relevant code paths:
  - `xmake/targets.lua`, `xmake/tests.lua`, `xmake/bench.lua`.
  - `include/oran/permission/`, `src/oran-permission/`,
    `tests/permission/`, `bench/permission/`.
- Constraints:
  - Pure stdlib; depends only on `oran-core`.
  - Public header pulls in `<cstdint>`, `<string>`, `<string_view>`,
    `<vector>`, `<format>` only.
  - `evaluate` is `noexcept` (allocates only when building the
    `Decision::reason`).
- Compile-budget impact (if any):
  - One small public header + one implementation TU under the
    `oran-permission` ≤ 2.5 s per-TU budget category (matches the
    "tool / memory / hook / skill" row in module-boundaries.md).

## Risks

- Risk: the simple glob matcher accepts patterns the re2 path will
  later reject (or vice versa). Mitigation: glob is documented to
  support only `*`; complex patterns belong on the future
  `InputPattern` regex hop.
- Risk: callers misuse the foundation by treating it as the full
  permission engine. Mitigation: the umbrella header is named
  `permission.hpp` (not `evaluator.hpp`), the doc note says explicitly
  what is missing, and the foundation's class is `RuleSet` — not
  `Evaluator`.

## Milestones

1. Add active plan.
2. Wire library + test + bench targets in xmake.
3. Implement types + matcher + evaluator.
4. Add tests.
5. Add bench scenario.
6. Update docs and write history.
7. Run validation and move plan to `completed/`.

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
  - Public header includes are stdlib-only.
  - Empty rule set with `Mode::strict` yields `deny`; `Mode::sandboxed`
    yields `deny`; `Mode::default_` yields `ask`; `Mode::permissive`
    and `Mode::auto_`-equivalent (named `permissive` in this slice)
    yield `allow`.
- Bench comparison (if perf-relevant):
  - `permission.rule_set_evaluate` vs. `permission.linear_find_if`
    over a 16-rule fixture. The precedence-respecting walk does up to
    three passes; the comparison documents the cost.

## Progress Log

- [x] Confirm scope.
- [x] Add library scaffolding.
- [x] Implement types + matcher + evaluator.
- [x] Add tests.
- [x] Add bench scenario.
- [x] Update docs and write history.
- [x] Run validation and move plan to `completed/`.

## Decision Log

- 2026-05-16: omit the `Mode::auto_` enumerator from the design doc and
  keep `permissive` as the "near-`auto`" mode for this slice. Rationale:
  `auto` in the spec is the channel-default mode; without channels
  landed there is no semantic difference between `auto` and
  `permissive`. Adding the enumerator just to leave it unused is dead
  code.

## Linked Artifacts

- Related design doc: `docs/design-docs/permissions-and-hooks.md`.
- Related product spec: `docs/product-specs/0008-permissions.md`.
- PRs: local change.
- History entry: `docs/histories/2026-05/<timestamp>-oran-permission-foundation.md`
- Release note: `docs/releases/feature-release-notes.md`
