## [2026-05-16 23:05] | Task: `oran-permission` three-layer `materialize` slice

### Execution Context

- Agent: Claude (Opus 4.7, fast mode, max effort)
- Base model: claude-opus-4-7
- Runtime: local xmake release build, GCC 16.1 baseline
- Linked plan: `docs/exec-plans/completed/2026-05-16-oran-permission-materialize.md`

### User Query

> wiring oran-config::permissions so the layer-2/3 merge has a config-side consumer.

(Second slice of the goal — the first slice landed the config-side
data, this slice consumes it.)

### Changes Overview

- Areas:
  - New public header
    `include/oran/permission/materialize.hpp` and TU
    `src/oran-permission/materialize.cpp` exposing
    `permission::materialize(Mode, const config::PermissionsConfig& global,
    const config::PermissionsConfig& per_agent) -> RuleSet`
    and a two-argument convenience overload.
  - Umbrella header `include/oran/permission.hpp` now re-exports
    `materialize.hpp` alongside `defaults.hpp` and
    `rule_set.hpp` so `<oran/permission.hpp>` stays the single
    consumer entry point.
  - `xmake/targets.lua` adds `oran-config` as a dep of
    `oran-permission`. The boundary direction is preserved —
    `oran-permission` (composition utilities layer) sits above
    `oran-config` (platform layer) per
    `docs/design-docs/module-boundaries.md`.
  - `tests/permission/test_materialize.cpp` covers the merge:
    empty config + empty overlay equals
    `Defaults::for_mode`; global rules append after defaults;
    per-agent overlay appends after global; verdict mapping
    is one-to-one; capability scope survives across the
    config-to-runtime boundary; deny in any layer outranks
    allow in any other layer (mirrored from both the global
    and per-agent sides); intra-layer order is preserved; the
    two-argument overload uses an empty per-agent overlay.
  - `bench/permission/scenarios/materialize.cpp` registers
    three scenarios sharing the same `Mode::default_`
    defaults: `permission.materialize_defaults_only`,
    `permission.materialize_with_global` (8-rule global),
    `permission.materialize_with_global_and_agent` (8-rule
    global + 2-rule overlay).
  - `bench/permission/main.cpp` wired with the new title
    block.
  - Architecture / quality / design-doc / product-spec /
    release-notes / bench-README updates.
- Key actions:
  - Implemented `materialize` as a pure concatenation:
    `Defaults::for_mode(mode)` first, then `global.rules` in
    source order, then `per_agent.rules` in source order.
    The runtime evaluator's deny → allow → ask precedence
    walk is unchanged, so an explicit deny in any layer
    outranks an allow in any other layer (matching the
    design-doc "explicit deny always wins" guarantee).
  - Internal TU-local helper `to_verdict(config::PermissionVerdict)`
    maps the config-side enum to the runtime
    `permission::Verdict`. The mapping is one-to-one today;
    keeping the function explicit documents that a future
    divergence (e.g. a config-only `defer` verdict) would
    show up here rather than silently leaking across.

### Design Intent

`docs/design-docs/permissions-and-hooks.md` ("Sources") names a
three-layer merge: built-in defaults → global config → per-agent
overlay. The first two slices owned the defaults and the parsed
config-side data; this slice is the runtime merge that combines
the three layers into a single `RuleSet` the existing evaluator
can consume.

The design doc's "later layers override earlier ones" phrasing
could be read two ways:

1. Pure concatenation, relying on the existing deny → allow →
   ask precedence walk to enforce the "explicit deny always
   wins over allow" guarantee.
2. Per-rule diff/merge, where later layers silently drop earlier
   layers' rules that match some key (verdict + pattern +
   capability).

We picked (1). The deny pass already runs before the allow pass,
so a later-layer `allow` of a tool already covered by an
earlier-layer `deny` still loses. A per-rule diff/merge would
silently drop rules and surprise operators reading the audit
log; a future `--explain-rules` CLI subcommand would also become
harder to write under shape (2) because the rule provenance
would be lost. The history captures this decision so the next
agent doesn't reopen it.

Adding `oran-config` as a dep of `oran-permission` is the first
upward edge from `oran-permission` into the platform layer (it
already depended on `oran-core` only). The module-boundary
diagram allows it — `oran-permission` is in the composition
utilities layer, which is above the platform layer — but it is
worth noting in the history that the public header now pulls in
`<oran/config/config.hpp>`. That header is stdlib-only on the
public side (its `nlohmann::json` is confined to the TU), so no
transitive heavy includes leak in.

The bench numbers on this host:

| scenario                                              | ns/RuleSet |
| ----------------------------------------------------- | ---------- |
| `permission.materialize_defaults_only`                | ~122 ns    |
| `permission.materialize_with_global` (8 rules)        | ~230 ns    |
| `permission.materialize_with_global_and_agent` (+2)   | ~257 ns    |

The per-rule append cost is ~14 ns/rule (tool_pattern string
copy + capability optional copy + `Rule` move into the
vector). The third scenario is what `oran-bootstrap`'s future
per-agent runtime assembly will pay for each agent at startup;
for any reasonable rule count it stays well under any latency
budget that matters.

### Files Modified

- `include/oran/permission/materialize.hpp` (new)
- `src/oran-permission/materialize.cpp` (new)
- `include/oran/permission.hpp` (umbrella adds `materialize.hpp`)
- `xmake/targets.lua` (`oran-permission` adds `oran-config` dep)
- `tests/permission/test_materialize.cpp` (new)
- `bench/permission/scenarios/materialize.cpp` (new)
- `bench/permission/main.cpp`
- `bench/permission/README.md`
- `docs/ARCHITECTURE.md`
- `docs/QUALITY_SCORE.md`
- `docs/design-docs/permissions-and-hooks.md`
- `docs/product-specs/0008-permissions.md`
- `docs/releases/feature-release-notes.md`
- `docs/exec-plans/active/2026-05-16-oran-permission-materialize.md`
  (new, moved to `completed/` at end of slice)

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/ARCHITECTURE.md` — slice-status note + `oran-permission`
  inventory row name the new `materialize` API and the now-live
  three-layer merge; `oran-permission` deps now read
  `oran-core`, `oran-config`.
- `docs/QUALITY_SCORE.md` — Permissions row narrates the new
  materializer, bumps the test/bench counters (30 cases / 114
  assertions), updates "Next Step" to point at re2 / HMAC /
  audit; test framework and bench harness rows updated.
- `docs/design-docs/permissions-and-hooks.md` — "Engine status"
  block now says the runtime merge is live end-to-end.
- `docs/product-specs/0008-permissions.md` — acceptance
  criterion #4 annotated "Foundation landed 2026-05-16" with
  the materializer pointer; re2 input regex remains open.
- `docs/releases/feature-release-notes.md` — added the
  `permission-materialize` row with the new API surface and
  bench numbers.
- `bench/permission/README.md` — scenarios table extended with
  the three-layer materialize block.

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
  - `tests/permission/test_materialize.cpp`: 8 new cases / 23
    new assertions (empty merge == defaults; global rules
    append after defaults; per-agent overlay appends after
    global; verdict mapping one-to-one; capability scope
    preserved; deny in any layer outranks allow in any other
    layer for both global and per-agent; intra-layer order;
    two-argument overload).
- Bench impact:
  - `bench/permission/scenarios/materialize.cpp` registers
    `permission.materialize_defaults_only` (~122 ns),
    `permission.materialize_with_global` (~230 ns), and
    `permission.materialize_with_global_and_agent` (~257 ns).
- Compile-budget delta:
  - One new small public header (re-exports
    `rule_set.hpp` + `<oran/config/config.hpp>`) + one new TU.
    The library stays well under the `oran-permission` ≤ 2.5 s
    per-TU budget.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none — re2 input regex, HMAC approvals,
  audit logging, and `oran-bootstrap` per-agent assembly are
  explicit downstream slices listed in
  `docs/product-specs/0008-permissions.md`.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-05`
