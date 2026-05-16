# `oran-core` — `Capability` Enum Slice

## Goal

Land the small follow-up `oran-core` data slice that introduces the
`Capability` enum: the v2 vocabulary that ties tools (`oran-tool`) to
permission rules (`oran-permission`). `Capability` belongs in
`oran-core` per `docs/design-docs/module-boundaries.md` (the "What Goes
In `oran-core`?" table explicitly lists "`Capability` enums" on the
"In core" side), so this slice owns only the *type* — the future
`Rule::capability` field lands in the next `oran-permission` slice, and
the `ToolDef::requires` list lands when `oran-tool` itself comes up.

## Scope

- In scope:
  - `include/oran/core/capability.hpp` and
    `src/oran-core/capability.cpp`: `core::Capability` enum (the
    18-entry list from `docs/design-docs/tool-runtime.md`), with
    `to_string_view(Capability) noexcept -> std::string_view`,
    `parse_capability(std::string_view) noexcept ->
    std::optional<Capability>`, an `kAllCapabilities` constexpr span
    for callers that want to iterate the universe, and
    `std::formatter<Capability>` so the value formats with
    `std::print` / `std::format` directly. No bitset alias yet — the
    set type comes with the permission/tool slices that actually
    consume it.
  - Test bucket: `tests/core/test_capability.cpp` — every enumerator
    round-trips via `to_string_view` + `parse_capability`, the
    formatter prints the same spelling, out-of-range values map to
    `"unknown"` for symmetry with `Role`/`StopReason`, unknown
    spellings (including the `"unknown"` sentinel itself) refuse to
    parse, and `kAllCapabilities` has the same count as the
    enumerator list.
  - Bench: `bench/core/scenarios/capability.cpp` registering
    `core.capability_parse_linear` (the `parse_capability`
    implementation — a constexpr-table linear scan) vs.
    `core.capability_parse_unordered_map` (an
    `std::unordered_map<std::string_view, Capability>` looked up by
    hash) over a deterministic set of spellings. Documents the cost
    of the table-scan path so the next caller picks with eyes open.
  - Docs: `docs/ARCHITECTURE.md` inventory + slice-status row,
    `docs/QUALITY_SCORE.md` test/bench rows, `bench/core/README.md`,
    `docs/releases/feature-release-notes.md`, and a history entry.
- Out of scope:
  - Adding `std::optional<Capability>` to `permission::Rule` (next
    slice; same exec-plan series).
  - Adding `std::vector<Capability> requires` to `core::ToolDef`.
    `ToolDef` already shipped without it; bringing it up is part of
    the future `oran-tool` slice that owns runtime capability
    enforcement.
  - A bitset-of-Capability typedef. The set type lives on the layer
    that consumes it (permission rule sets, tool runtime granted
    sets), and locking the representation here would force one
    choice prematurely.
  - Wiring `Capability` into `oran-config`.

## Context

- Relevant docs:
  - `docs/design-docs/tool-runtime.md` (canonical `Capability` enum
    text).
  - `docs/design-docs/permissions-and-hooks.md` (consumer side:
    capability-aware gating in `Rule`).
  - `docs/design-docs/module-boundaries.md` ("`Capability` enums" →
    `oran-core`).
  - `docs/rules/critical-rules.md` (C7 — explicit ctors; C17 —
    modern stdlib facilities).
- Relevant code paths:
  - `include/oran/core/role.hpp` + `src/oran-core/role.cpp` — the
    closest existing template (enum + stable string mapping +
    parse + formatter).
  - `include/oran/core/stop_reason.hpp` — read-only enum without a
    parse helper, kept here for completeness reference.
  - `bench/core/scenarios/`, `tests/core/`.
- Constraints:
  - Pure stdlib; no new third-party packages.
  - Public header pulls in `<cstdint>`, `<format>`, `<optional>`,
    `<span>`, `<string_view>` only.
  - `to_string_view` and `parse_capability` are `noexcept`.
- Compile-budget impact (if any):
  - One small public header + one implementation TU + one test TU +
    one bench scenario TU. All inside the `oran-core` ≤ 1.5 s per-TU
    budget.

## Risks

- Risk: someone treats the enum's underlying integer values as
  stable wire identifiers and the order shifts under us. Mitigation:
  the public surface is name-based (`to_string_view` /
  `parse_capability`); the enum carries the documented
  `std::uint8_t` underlying type so the storage is fixed even if the
  enumerator order is rearranged later.
- Risk: the next consumer (permission) wants a set type but every
  caller picks a different representation (bitset / sorted vector /
  flat-set). Mitigation: this slice deliberately leaves the set
  representation to the consumer that needs it. The 18-entry
  universe is small enough that a `std::uint32_t` bitset works
  later; revisiting the set type in the permission slice keeps the
  cost of that decision local.

## Milestones

1. Add active plan, settle the enumerator list and parse contract.
2. Implement `Capability` header + TU.
3. Add focused tests.
4. Add bench scenario + register.
5. Update docs/history/release notes.
6. Run validation and move plan to `completed/`.

## Validation

- Commands:
  - `xmake build oran-core`
  - `xmake build test-core && xmake run test-core`
  - `xmake build bench-core && xmake run bench-core`
  - `xmake test`
  - `xmake build orangutan`
  - `git diff --check`
  - `make ci`
- Manual checks:
  - Public header includes the stdlib-only list above and nothing
    else.
  - `parse_capability(to_string_view(c))` round-trips every
    enumerator in the test.
- Observability checks:
  - None (pure value type).
- Bench comparison (if perf-relevant):
  - `core.capability_parse_linear` vs.
    `core.capability_parse_unordered_map` — documents whether the
    table-scan path is justified at this size.

## Progress Log

- [x] Confirm scope and constraints.
- [x] Implement `Capability`.
- [x] Add tests.
- [x] Add bench scenario.
- [x] Update docs.
- [x] Run validation.
- [x] Write history entry.
- [x] Add release note.
- [x] Move plan to `completed/`.

## Decision Log

- 2026-05-16: keep `Capability` in `oran-core` rather than
  `oran-permission`. Rationale:
  `docs/design-docs/module-boundaries.md` explicitly places
  "`Capability` enums" on the `oran-core` side of the boundary table.
  Both `oran-permission` (rule scope) and the future `oran-tool`
  (declared `requires`, granted-set) read the type; placing it in
  core avoids a `permission → tool` or `tool → permission` link.
- 2026-05-16: ship a constexpr `kAllCapabilities` span instead of a
  bitset alias. Rationale: tests, future `--explain-rules` CLI, and
  schema generation will want to iterate the universe; locking the
  set representation belongs on the consumer.

## Linked Artifacts

- Related design doc: `docs/design-docs/tool-runtime.md`,
  `docs/design-docs/permissions-and-hooks.md`,
  `docs/design-docs/module-boundaries.md`.
- Related product spec: `docs/product-specs/0008-permissions.md`
  (capability gating is part of v1).
- PRs: local change.
- History entry: `docs/histories/2026-05/<timestamp>-oran-core-capability.md`
- Release note: `docs/releases/feature-release-notes.md`
