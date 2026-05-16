## [2026-05-16 21:20] | Task: `oran-permission` `Defaults` baseline factory

### Execution Context

- Agent: Claude (Opus 4.7, fast mode, max effort)
- Base model: claude-opus-4-7
- Runtime: local xmake release build, GCC 16.1 baseline
- Linked plan: `docs/exec-plans/completed/2026-05-16-oran-permission-defaults.md`

### User Query

> 详细了解项目目标，查看当前项目进度, 推进项目代码的实现.

### Changes Overview

- Areas:
  - New `oran-permission` public surface:
    `include/oran/permission/defaults.hpp` and
    `src/oran-permission/defaults.cpp` exposing
    `Defaults::for_mode(Mode) -> RuleSet`.
  - Umbrella header `include/oran/permission.hpp` now re-exports
    `defaults.hpp` alongside `rule_set.hpp` so consumers keep
    using `<oran/permission.hpp>` as the single entry point.
  - `tests/permission/test_defaults.cpp` covering every mode's
    baseline shape (rule count + verdict per capability),
    capability-less fall-back, reason annotation through the
    factory build, and referential transparency.
  - `bench/permission/scenarios/defaults.cpp` registering
    `permission.defaults_build_default` (factory) vs.
    `permission.defaults_hand_built_default` (inline) and
    `bench/permission/main.cpp` wired with the new title block.
  - Architecture / quality / design-doc / release-notes /
    bench-README updates.
- Key actions:
  - Implemented `Defaults::for_mode` with a `switch` over `Mode`
    that returns:
    - `Mode::strict` → empty `RuleSet`.
    - `Mode::default_` → 9 capability-scoped rules
      (deny `runtime_loader` + `delete_path`; allow `read_file`
      + `read_memory`; ask `write_file`, `edit_file`,
      `write_memory`, `spawn_subprocess`, `egress_http`).
    - `Mode::permissive` → 2 denies (`runtime_loader`,
      `delete_path`) — the mode's default verdict handles
      everything else.
    - `Mode::sandboxed` → 2 allows (`read_file`, `read_memory`)
      — the mode's default verdict denies everything else.
    - Default branch (unknown enum value via cast) → empty
      `RuleSet`. The mode's default verdict still applies.
  - All baseline rules use `tool_pattern = "*"` and a
    `capability` scope so they survive tool renames (per the
    design-doc capability-aware-gating guidance).
  - Internal helpers `allow(Capability)`, `ask(Capability)`,
    `deny(Capability)` keep the per-mode baselines readable
    without exposing new public surface.

### Design Intent

`docs/design-docs/permissions-and-hooks.md` says rules come from
three layers: defaults → global config → per-agent overlay. The
foundation slice landed without layer 1; this slice adds it. The
runtime *merge* of all three layers belongs on the
`oran-config::permissions` slice once the config side knows how to
materialize a `RuleSet` — without that consumer, a merge helper
here would be code without a caller.

Per-mode baselines are opinions about which capabilities are
"safe to read", "ask-worthy", or "outright dangerous". The choices
encode the safe defaults of the design doc's mode table:

- `Mode::strict` ships *empty* defaults on purpose. Strict mode's
  defining property is "deny by default, allow only explicit"; a
  strict mode that ships pre-allowed capabilities contradicts
  itself. Operators who want a strict baseline with read-side
  capabilities allowed pick `Mode::sandboxed` instead.
- `Mode::default_` is the only mode with a substantive baseline —
  it allows read-side, asks for the high-impact write side, and
  denies what would let an agent loader compromise the runtime
  (`runtime_loader`) or wipe disk state (`delete_path`). This
  closely follows the design doc's example rules section.
- `Mode::permissive` adds *only* the two deny rules; the mode's
  default verdict allows everything else.
- `Mode::sandboxed` adds *only* the two allow rules; the mode's
  default verdict denies everything else.

The bench numbers on this host:

| scenario                                  | ns/RuleSet |
| ----------------------------------------- | ---------- |
| `permission.defaults_build_default`       | ~113 ns    |
| `permission.defaults_hand_built_default`  | ~112 ns    |

The factory is essentially free vs. inline construction (~1.4 %
spread, well inside noise). That matches expectations — the factory
adds one function-call frame and one `RuleSet` move; both vanish
under the cost of nine `std::vector::push_back` calls. Future
config-loading code can call the factory at startup without
inventing a separate path.

### Files Modified

- `include/oran/permission/defaults.hpp` (new)
- `src/oran-permission/defaults.cpp` (new)
- `include/oran/permission.hpp` (umbrella adds `defaults.hpp`)
- `tests/permission/test_defaults.cpp` (new)
- `bench/permission/scenarios/defaults.cpp` (new)
- `bench/permission/main.cpp`
- `bench/permission/README.md`
- `docs/ARCHITECTURE.md`
- `docs/QUALITY_SCORE.md`
- `docs/design-docs/permissions-and-hooks.md`
- `docs/releases/feature-release-notes.md`
- `docs/exec-plans/active/2026-05-16-oran-permission-defaults.md`
  (new, moved to `completed/` at end of slice)

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/ARCHITECTURE.md` — slice-status note + `oran-permission`
  inventory row now name the new `Defaults::for_mode` baseline
  factory.
- `docs/QUALITY_SCORE.md` — permissions row narrates the new
  surface and bumps the test/bench counters (22 / 91); bench row
  mentions the new `Defaults` A/B; test framework row updated.
- `docs/design-docs/permissions-and-hooks.md` — "Engine status"
  block now points at the layer-1 baseline; layer-2 and layer-3
  wiring stay called out as downstream.
- `docs/releases/feature-release-notes.md` — added the
  `permission-defaults` row with the new API surface, the
  per-mode shape, and the bench numbers.
- `bench/permission/README.md` — scenarios table extended with
  the factory-vs-inline A/B description.

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
  - `tests/permission/test_defaults.cpp`: 7 new cases / 32 new
    assertions (per-mode baseline shape, capability-less call
    fall-back, capability-scoped reason annotation, referential
    transparency).
- Bench impact:
  - `bench/permission/scenarios/defaults.cpp` registers
    `permission.defaults_build_default` (~113 ns) vs.
    `permission.defaults_hand_built_default` (~112 ns) — the
    factory adds no measurable overhead.
- Compile-budget delta:
  - One new small public header (re-exports
    `rule_set.hpp` only) + one new TU. The library stays well
    under the `oran-permission` ≤ 2.5 s per-TU budget.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none — the runtime merge that combines
  defaults + config + per-agent overlay is explicitly the
  next slice's job once `oran-config::permissions` lands.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-05`
