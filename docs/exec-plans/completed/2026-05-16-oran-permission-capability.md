# `oran-permission` — Capability Gating Slice

## Goal

Wire `core::Capability` (just landed in the previous slice) into
`permission::RuleSet::evaluate` so a rule can scope to a capability,
matching only when the invocation declares that capability among the
tool's required set. This closes acceptance criterion #3 of
`docs/product-specs/0008-permissions.md`:

> Capability mismatch is enforced — a tool that didn't declare
> `Capability::network` cannot use it even if a rule otherwise
> allowed.

…against the foundation slice that landed without capability support.
Re2 input regex, HMAC approvals, audit logging, and config wiring
remain downstream.

## Scope

- In scope:
  - Extend `permission::Rule` with `std::optional<core::Capability>
    capability` (default `std::nullopt` — rule remains unscoped).
  - Add a capability-aware overload to `permission::RuleSet`:
    ```cpp
    Decision evaluate(std::string_view tool_name,
                      std::span<const core::Capability> required_capabilities,
                      Mode mode) const;
    ```
    Keep the existing `evaluate(tool_name, mode)` overload as a
    thin wrapper that calls the new one with an empty span.
  - Rule-matching predicate: a rule matches `tool_name` iff the
    glob matches AND
    (`!rule.capability.has_value()` OR
    `std::ranges::find(required_capabilities, *rule.capability) !=
    required_capabilities.end()`).
  - Reason formatting now appends `" capability=<name>"` when the
    matching rule had a capability scope, so the `Decision::reason`
    stays a useful "why" for future `--explain-rules`.
  - Tests: capability-bound deny outranks capability-bound allow
    (same precedence ordering), capability mismatch falls through
    to the next precedence pass, capability mismatch lands on mode
    default when no rule fires, unscoped rules still match when
    the call passes capabilities, capability scope round-trips in
    the reason string.
  - Bench: extend `bench/permission/scenarios/rule_set.cpp` with a
    new A/B — `permission.rule_set_capability_match` (capability-
    bound rule that fires) vs. `permission.rule_set_capability_miss`
    (capability-bound rule that the call's required set excludes,
    falling through to the mode default). The existing
    `rule_set_evaluate` vs. `linear_find_if` scenario stays for
    continuity.
  - Docs: `docs/ARCHITECTURE.md` `oran-permission` row mentions
    capability gating; `docs/QUALITY_SCORE.md` permissions /
    test / bench rows; `docs/design-docs/permissions-and-hooks.md`
    "Engine status" note updates to reflect the new shape and what
    is still downstream; `docs/product-specs/0008-permissions.md`
    acceptance-criterion #3 gains a "(slice landed 2026-05-16)"
    annotation; release notes; history entry.
- Out of scope:
  - `re2`-based input regex matching on `Rule`.
  - HMAC-signed approval prompts and replay TTL.
  - Audit log writes to `audit.db`.
  - `oran-config` wiring (`config.permissions`).
  - Adding `std::vector<core::Capability> requires` to
    `core::ToolDef`. That belongs on the future `oran-tool` slice
    that owns runtime enforcement; the permission engine accepts
    whatever the caller passes regardless.
  - Per-channel overlays, time-bound approvals, sticky approvals.

## Context

- Relevant docs:
  - `docs/product-specs/0008-permissions.md` (#3 = capability
    enforcement; this slice closes it).
  - `docs/design-docs/permissions-and-hooks.md` (`Rule` shape and
    "Capability-Aware Gating" section dictate the semantics).
  - `docs/design-docs/tool-runtime.md` (`Capability` enum,
    `requires` list convention).
- Relevant code paths:
  - `include/oran/permission/rule_set.hpp`,
    `src/oran-permission/rule_set.cpp`.
  - `include/oran/core/capability.hpp` (just landed).
  - `tests/permission/test_rule_set.cpp`,
    `bench/permission/scenarios/rule_set.cpp`.
- Constraints:
  - `oran-permission` may depend only on `oran-core` (already
    true; the new include is `<oran/core/capability.hpp>`).
  - Public header gains `<optional>` and `<span>` — both stdlib,
    rule C6 unaffected.
  - The new overload is `noexcept(false)` only insofar as it
    formats the reason string; the matching pass itself is
    allocation-free.
- Compile-budget impact (if any):
  - One small public-header change, one TU change. The library
    stays well under the `oran-permission` ≤ 2.5 s per-TU budget
    listed in `module-boundaries.md`.

## Risks

- Risk: silently changing the semantics of an unscoped rule when
  the caller passes capabilities. Mitigation: unscoped rules
  (`!rule.capability.has_value()`) always match, regardless of
  what the caller passes — verified with a test.
- Risk: the existing single-arg `evaluate(tool_name, mode)`
  callsites (tests + bench) start drifting from the canonical
  capability-aware shape. Mitigation: keep both overloads; the
  single-arg one calls the new one with `{}` and documents that it
  is the "no capability information" shortcut.
- Risk: callers confuse "the rule's capability scope" with "the
  tool's required capability list". Mitigation: parameter name
  `required_capabilities`, doc comment, and the design doc's
  "Capability-Aware Gating" section all say the same thing.

## Milestones

1. Land plan, settle the matching predicate.
2. Extend `Rule` + `RuleSet`, add the new overload + reason
   formatting.
3. Extend tests.
4. Extend bench.
5. Update docs/history/release notes.
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
  - Public header still avoids heavy includes.
  - The single-arg `evaluate` overload still passes for callers
    that don't reach for capabilities yet.
- Bench comparison (if perf-relevant):
  - `permission.rule_set_capability_match` (rule fires) vs.
    `permission.rule_set_capability_miss` (rule scoped out, falls
    through to mode default) — documents the cost of the extra
    scan over the rule's optional capability against the path that
    bails on the first capability check.

## Progress Log

- [x] Confirm scope.
- [x] Implement Rule extension + capability-aware overload.
- [x] Update tests.
- [x] Update bench.
- [x] Update docs.
- [x] Run validation.
- [x] Write history entry.
- [x] Add release note.
- [x] Move plan to `completed/`.

## Decision Log

- 2026-05-16: keep the existing `evaluate(tool_name, mode)`
  overload as a wrapper. Rationale: the tests + bench already use
  it heavily and the wrapper costs nothing; a capability-aware
  caller that forgets to pass the list still gets the unscoped
  semantics they used to.
- 2026-05-16: include the matched capability in the `Decision::
  reason` only when the rule was scoped. Rationale: unscoped rules
  produce shorter reasons and the future `--explain-rules` CLI
  wants the capability mentioned exactly when it was load-bearing.

## Linked Artifacts

- Related design doc:
  `docs/design-docs/permissions-and-hooks.md`,
  `docs/design-docs/tool-runtime.md`.
- Related product spec: `docs/product-specs/0008-permissions.md`.
- PRs: local change.
- History entry: `docs/histories/2026-05/<timestamp>-oran-permission-capability.md`
- Release note: `docs/releases/feature-release-notes.md`
