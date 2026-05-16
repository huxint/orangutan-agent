## [2026-05-16 20:30] | Task: `oran-permission` capability gating slice

### Execution Context

- Agent: Claude (Opus 4.7, fast mode, max effort)
- Base model: claude-opus-4-7
- Runtime: local xmake release build, GCC 16.1 baseline
- Linked plan: `docs/exec-plans/completed/2026-05-16-oran-permission-capability.md`

### User Query

> 详细了解项目目标，查看当前项目进度, 推进项目代码的实现.

### Changes Overview

- Areas:
  - `oran-permission` public header gains `Rule::capability`
    (`std::optional<core::Capability>`) and a new
    `RuleSet::evaluate(tool_name, required_capabilities, mode)`
    overload.
  - Reason formatting annotates `capability=<name>` only when the
    firing rule had a scope.
  - `tests/permission` extended with a new test file dedicated to
    capability semantics; existing `test_rule_set.cpp` updated to
    explicitly initialize the new optional field so the
    `-Wmissing-field-initializers` warning the schema change
    introduced stays at zero.
  - `bench/permission` extended with a separate 16-rule
    capability-scoped fixture and two new A/B scenarios
    (`permission.rule_set_capability_match` vs.
    `permission.rule_set_capability_miss`).
  - Architecture / quality / design-doc / product-spec /
    release-notes updates.
- Key actions:
  - Added `Rule::capability` with `std::optional<core::Capability>`
    default `std::nullopt` (preserves unscoped behavior for every
    existing rule).
  - Added the capability-aware `evaluate` overload. The matching
    predicate is "glob matches AND (no capability scope OR scope
    appears in `required_capabilities`)". The legacy
    `evaluate(tool_name, mode)` overload becomes a thin wrapper
    that calls the new one with an empty span — capability-bound
    rules silently miss on that path, which is exactly the
    design-doc guarantee.
  - `format_reason` helper now branches on `rule.capability.
    has_value()` and renders
    `"rule #N (verdict: pattern capability=name)"` when the rule
    is scoped; unscoped rules keep the short reason.
  - Wrote `tests/permission/test_capability.cpp` with 7 cases / 18
    new assertions (capability match, miss, legacy-overload
    skipping, deny outranks allow at same scope, mismatch fall-
    through to next precedence pass, unscoped rule still matches
    when call passes capabilities, reason annotation round-trip).
  - Extended `bench/permission/scenarios/rule_set.cpp` with
    `make_capability_fixture()` (16 rules, half scoped) and two
    scenarios that compare a successful capability-scoped match
    against a miss that falls through to the mode default.

### Design Intent

`docs/product-specs/0008-permissions.md` acceptance criterion #3
is "capability mismatch is enforced — a tool that didn't declare
`Capability::network` cannot use it even if a rule otherwise
allowed". The foundation slice landed without that enforcement; this
slice closes it for the in-process rule-set surface. Config wiring,
HMAC approvals, and audit logging remain downstream.

The two interesting design choices:

1. **Keep the legacy overload.** Callers that don't yet plumb
   capabilities (most tests, the bench's classic A/B, future
   stub tooling) keep working — the wrapper passes an empty span
   so capability-bound rules naturally do not fire. That happens
   to be the exact semantics the design doc demands: a tool
   missing the required capability cannot use it even if a rule
   otherwise allowed. Same statement, two paths.
2. **Annotate the reason only when the rule was scoped.** The
   future `--explain-rules` CLI and the audit log both want to
   show whether a rule's capability scope was load-bearing.
   Unscoped rules keep the shorter reason so the audit log
   stays readable for the common case.

The set-membership check (`std::ranges::find` over the call's
`required_capabilities` span) is O(N · M) where N is the number of
rules and M is the size of the capability set. M is small (the
design-doc example shows 1–2 required capabilities per call) and
the bench fixture exercises a span of 2, so the cost stays linear
in N — the same big-O as the existing precedence walk.

Bench numbers on this host:

| scenario                                  | ns/evaluate |
| ----------------------------------------- | ----------- |
| `permission.rule_set_evaluate`            | ~139 ns     |
| `permission.linear_find_if`               | ~75 ns      |
| `permission.rule_set_capability_match`    | ~163 ns     |
| `permission.rule_set_capability_miss`     | ~118 ns     |

The capability-aware path costs ~17% more on a successful match
(the extra optional check + the longer reason format) and ~15%
less on a miss (no `rule #N` reason — the fallback `default by
mode=` string is shorter). Both numbers are well below the cost of
a real tool call, so the precedence-walk approach holds.

### Files Modified

- `include/oran/permission/rule_set.hpp`
- `src/oran-permission/rule_set.cpp`
- `tests/permission/test_rule_set.cpp` (added explicit
  `.capability = std::nullopt` to existing rules to silence the
  schema-change-induced `-Wmissing-field-initializers`)
- `tests/permission/test_capability.cpp` (new)
- `bench/permission/scenarios/rule_set.cpp` (new fixture +
  scenarios, plus the same explicit-nullopt cleanup on existing
  rules)
- `bench/permission/README.md`
- `docs/ARCHITECTURE.md`
- `docs/QUALITY_SCORE.md`
- `docs/design-docs/permissions-and-hooks.md`
- `docs/product-specs/0008-permissions.md`
- `docs/releases/feature-release-notes.md`
- `docs/exec-plans/active/2026-05-16-oran-permission-capability.md`
  (new, moved to `completed/` at end of slice)

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/ARCHITECTURE.md` — `oran-permission` slice-status line and
  inventory row now name capability-aware gating.
- `docs/QUALITY_SCORE.md` — permissions row narrates the new shape
  and what is still downstream; test framework row updated to 15 /
  59; bench row mentions the capability match/miss A/B.
- `docs/design-docs/permissions-and-hooks.md` — "Engine status"
  block updated: capability gating is on, the new overload exists,
  re2 / HMAC / audit / config wiring remain downstream.
- `docs/product-specs/0008-permissions.md` — acceptance criterion
  #3 annotated "(Foundation landed 2026-05-16: …)" with a note
  about the `Capability::network` spelling-vs-enum mismatch.
- `docs/releases/feature-release-notes.md` — added the
  `permission-capability` row with the new API surface, what
  remains downstream, and the bench numbers.
- `bench/permission/README.md` — scenarios table extended with
  the capability match/miss A/B description.

### Validation

- Commands run:
  ```sh
  xmake build oran-permission
  xmake build test-permission && xmake run test-permission
  xmake build bench-permission && xmake run bench-permission
  xmake build orangutan
  xmake test
  git diff --check
  make ci
  ```
- Tests added/changed:
  - `tests/permission/test_capability.cpp`: 7 new cases / 18 new
    assertions (capability match, miss, legacy-overload skipping,
    deny outranks allow at same scope, mismatch fall-through,
    unscoped rule still matches when call passes capabilities,
    reason annotation round-trip).
  - `tests/permission/test_rule_set.cpp`: every `Rule{...}` literal
    gains `.capability = std::nullopt` so the schema-change
    warning stays at zero.
- Bench impact:
  - `bench/permission/scenarios/rule_set.cpp`:
    `permission.rule_set_capability_match` (~163 ns) and
    `permission.rule_set_capability_miss` (~118 ns) over a separate
    16-rule capability-scoped fixture, alongside the existing
    precedence-walk vs. find_if pair.
- Compile-budget delta:
  - One small public-header edit (adds `<optional>` and `<span>`
    and `<oran/core/capability.hpp>` — all already on the PCH list
    via stdlib + the prior `core::Capability` slice). One TU
    edit. One new test TU. The library stays well under the
    `oran-permission` ≤ 2.5 s per-TU budget.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none — the remaining acceptance criteria
  (re2 input regex, HMAC approvals, audit logging, config wiring)
  are tracked under future slices of
  `docs/product-specs/0008-permissions.md`.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-05`
