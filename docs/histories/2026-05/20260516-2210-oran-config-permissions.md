## [2026-05-16 22:10] | Task: `oran-config` `permissions` + `agents.<name>.permissions` slice

### Execution Context

- Agent: Claude (Opus 4.7, fast mode, max effort)
- Base model: claude-opus-4-7
- Runtime: local xmake release build, GCC 16.1 baseline
- Linked plan: `docs/exec-plans/completed/2026-05-16-oran-config-permissions.md`

### User Query

> wiring oran-config::permissions so the layer-2/3 merge has a config-side consumer.

### Changes Overview

- Areas:
  - New typed surface in `include/oran/config/config.hpp`:
    `PermissionVerdict` enum + `to_string_view` /
    `parse_permission_verdict`, `PermissionRuleConfig`,
    `PermissionsConfig`, `AgentConfig`, and
    `Config::permissions()` / `Config::agents()` accessors.
  - Parser updates in `src/oran-config/config.cpp`:
    - Verdict-keyed `permissions.{allow,deny,ask}` arrays of
      rule objects `{tool_pattern, capability?}`.
    - `agents.<name>.permissions` overlays parse with the same
      machinery; unknown agent fields warn (loose) or fail
      (strict) per `strict_config`.
    - Capability strings resolve through `core::parse_capability`
      at load time; unknown spellings always error.
    - Switched the implementation from `nlohmann::json` to
      `nlohmann::ordered_json` so JSON authoring order survives
      across verdict keys and agent names (otherwise the
      `std::map`-backed default would alphabetize the keys).
  - `config.example.json` now carries a representative
    `permissions` block (matching the design-doc example) and an
    example `agents.researcher.permissions` overlay so the file
    documents the new schema.
  - `tests/config/test_config.cpp`: 7 new cases / 71 new
    assertions (verdict round-trip, populated permissions block,
    cross-verdict authoring order, agent overlay parsing, env
    substitution through permission strings, malformed-rule
    error paths, unknown verdict/rule/agent field handling per
    strict mode).
  - `bench/config/scenarios/permissions.cpp` registers
    `config.parse_permissions_empty` and
    `config.parse_permissions_typed` (16-rule + one-overlay
    fixture).
  - `bench/config/main.cpp` adds the matching title block.
  - Architecture / quality / design-doc / release-notes /
    bench-README updates.

### Design Intent

The design doc names a three-layer rule merge: defaults → global
config → per-agent overlay. Layer 1 (`Defaults::for_mode`)
landed in the previous slice. This slice owns layer-2 and the
data side of layer-3 by teaching `oran-config` how to parse the
`permissions` and `agents.<name>.permissions` JSON blocks into
typed structs. The runtime merge that combines all three layers
into a single `RuleSet` is intentionally the next slice — it
lives in `oran-permission` (above `oran-config` in the layer
ordering) and now has a real config-side consumer to read from.

`PermissionVerdict` is a separate enum from `permission::Verdict`
because `oran-config` sits below `oran-permission` in the
dependency graph; importing the permission header here would
reverse the layering rule. The values are identical and the
materializer will map one-to-one. The same reasoning applies to
keeping the capability validation eager at config-load time:
`core::Capability` lives in `oran-core` (below both layers), so
the config parser can resolve spellings without crossing the
boundary, and the materializer becomes a pure type-mapping step.

Switching to `nlohmann::ordered_json` was deliberate. With the
default `std::map`-backed JSON, iterating an object like
`{"ask": [...], "allow": [...], "deny": [...]}` produces
allow → ask → deny, alphabetized — which silently reorders
the operator's authoring intent. Ordered iteration costs O(n)
lookup instead of O(log n) but at the config-object scale
(< 16 keys per nested block) the difference vanishes. The
populated-permissions parse benchmarks at ~18 µs for a 16-rule
fixture + one agent overlay; the empty-block lift is ~879 ns,
roughly the cost of running the now-always-on permissions /
agents branches through `Config::parse`.

Unknown verdict keys, unknown rule fields, and unknown agent
fields all follow the existing root-unknown-fields policy:
silently warn in loose mode, hard-fail under `strict_config`.
This keeps the schema discoverable (typos still surface) while
leaving room for future expansion (e.g. agents will eventually
grow `model`, `prompts`, `hooks`, etc.) without forcing every
config file to be touched.

The bench numbers on this host:

| scenario                              | ns/parse |
| ------------------------------------- | -------- |
| `config.parse_permissions_empty`      | ~879 ns  |
| `config.parse_permissions_typed`      | ~18.4 µs |

The typed path is dominated by `nlohmann::json` allocation +
string copies. For startup-time parsing this is comfortably
below any reasonable budget; if it later becomes a hot path
we'd revisit by pre-allocating the rule vector or switching to
a streaming reader.

### Files Modified

- `include/oran/config/config.hpp`
- `src/oran-config/config.cpp`
- `config.example.json`
- `tests/config/test_config.cpp`
- `bench/config/scenarios/permissions.cpp` (new)
- `bench/config/main.cpp`
- `bench/config/README.md`
- `docs/ARCHITECTURE.md`
- `docs/QUALITY_SCORE.md`
- `docs/design-docs/permissions-and-hooks.md`
- `docs/releases/feature-release-notes.md`
- `docs/exec-plans/active/2026-05-16-oran-config-permissions.md`
  (new, moved to `completed/` at end of slice)

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/ARCHITECTURE.md` — slice-status note + `oran-config`
  inventory row + Configuration section name the new
  `permissions` and `agents.<name>.permissions` typed
  surfaces.
- `docs/QUALITY_SCORE.md` — Config row narrates the new
  surface; test framework counter for config bumps to 12 / 120;
  bench harness row mentions the new A/B.
- `docs/design-docs/permissions-and-hooks.md` — "Engine status"
  block now points at layer-2 / layer-3 *data* being parsed;
  only the runtime merge remains.
- `docs/releases/feature-release-notes.md` — added the
  `config-permissions` row with the new API surface, validation
  matrix, and bench numbers.
- `bench/config/README.md` — scenarios split into two blocks;
  permissions A/B documented.

### Validation

- Commands run:
  ```sh
  xmake build oran-config
  xmake build test-config && xmake run test-config
  xmake build bench-config && xmake run bench-config
  xmake build orangutan
  xmake test
  git diff --check
  ```
- Tests added/changed:
  - `tests/config/test_config.cpp`: 7 new cases / 71 new
    assertions covering verdict round-trip, populated
    permissions block parsing, cross-verdict authoring order
    preservation, agent overlay parsing, env substitution
    through permission strings, malformed-rule errors, and
    unknown verdict/rule/agent field handling per strict mode.
- Bench impact:
  - `bench/config/scenarios/permissions.cpp` registers
    `config.parse_permissions_empty` (~879 ns) vs.
    `config.parse_permissions_typed` (~18.4 µs).
- Compile-budget delta:
  - One new struct group in the public header (stdlib-only
    plus `core::Capability`) + one new bench TU. The
    implementation TU stays well under the `oran-config` ≤ 2.0 s
    budget.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none — the runtime merge that combines
  `Defaults::for_mode` + `Config::permissions` +
  `Config::agents` is the next planned slice.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-05`
