## [2026-05-16 19:00] | Task: `oran-permission` foundation slice

### Execution Context

- Agent: Claude (Opus 4.7, fast mode, max effort)
- Base model: claude-opus-4-7
- Runtime: local xmake release build, GCC 16.1 baseline
- Linked plan: `docs/exec-plans/completed/2026-05-16-oran-permission-foundation.md`

### User Query

> 详细了解项目目标，查看当前项目进度, 推进项目代码的实现.

### Changes Overview

- Areas:
  - New library `oran-permission` (xmake target, public header,
    implementation TU, tests bucket, bench bucket).
  - Architecture / quality / design-doc / release-notes updates.
- Key actions:
  - Added `permission::Verdict { allow, deny, ask }` and
    `permission::Mode { strict, default_, permissive, sandboxed }`
    enums with `to_string_view` mappings and `std::formatter`
    specializations.
  - Added `permission::Rule`, `permission::Decision`, and
    `permission::RuleSet` (with `add` / `clear` / `size` /
    `evaluate(tool_name, mode)`).
  - Implemented the deny → allow → ask precedence walk from
    `docs/design-docs/permissions-and-hooks.md`. When nothing matches,
    the fallback verdict is determined by `Mode` (`strict`/`sandboxed`
    → deny, `default_` → ask, `permissive` → allow).
  - Implemented a textbook `*`-glob matcher (`permission::glob_match`)
    with iterative backtracking. No `?`, no character classes — those
    belong on the future `InputPattern` regex hop.
  - Set up `tests/permission/main.cpp` + `test_rule_set.cpp` (8 cases
    / 41 assertions) covering literal match, `*` wildcard, deny-wins
    precedence, allow-first match, ask precedence, default-by-mode for
    every mode, empty rule set, and the indexed-reason output.
  - Set up `bench/permission/main.cpp` + `scenarios/rule_set.cpp`
    registering `permission.rule_set_evaluate` vs.
    `permission.linear_find_if` over a 16-rule fixture.
  - Wired `xmake/targets.lua`, `xmake/tests.lua`, `xmake/bench.lua`,
    and the `orangutan` binary's `add_deps` to pick up the new lib.

### Design Intent

The permissions row in `docs/QUALITY_SCORE.md` was at `D` — engine
designed, no implementation. The scope of the v1 spec
(`docs/product-specs/0008-permissions.md`) is huge: re2 input regex,
capability gating, HMAC-signed approval prompts, audit log writes.
Landing the whole thing in one slice would violate rule C14. Instead
this slice ships the *foundation* — the verdict vocabulary, the
mode-driven default verdict, the rule shape, the rule evaluator with
the design-doc precedence — so the row moves to `C` and future
slices can iterate on the same surface.

The `glob_match` matcher intentionally supports only `*`. The product
spec's "shell.exec(/bin/{ls,cat,...}:*)" patterns require the re2 path,
which is its own slice. Tool-name globbing is the most common shape and
the cheapest thing to land first.

`Decision` exposes a human-readable `reason` that names the matching
rule index or the falling-back mode. That's what the future audit log
and CLI `--explain-rules` subcommand will format on top of.

The bench result on this host — `permission.rule_set_evaluate`
~156 ns vs. `permission.linear_find_if` ~83 ns over a 16-rule
fixture — documents the cost of the precedence-respecting walk
(up to three passes + a formatted-reason build) over the cheapest
possible single-pass matcher. Both numbers are well below the cost of
an actual tool call; the precedence walk is not a hot-path problem.

### Files Modified

- `include/oran/permission.hpp` (new umbrella header)
- `include/oran/permission/rule_set.hpp`
- `src/oran-permission/rule_set.cpp`
- `tests/permission/main.cpp`
- `tests/permission/test_rule_set.cpp`
- `bench/permission/main.cpp`
- `bench/permission/scenarios/rule_set.cpp`
- `bench/permission/README.md`
- `xmake/targets.lua`
- `xmake/tests.lua`
- `xmake/bench.lua`
- `docs/ARCHITECTURE.md`
- `docs/QUALITY_SCORE.md`
- `docs/design-docs/permissions-and-hooks.md`
- `docs/releases/feature-release-notes.md`
- `docs/exec-plans/active/2026-05-16-oran-permission-foundation.md`
  (moved to `completed/`)

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/ARCHITECTURE.md` — slice-status note + `oran-permission` row
  now reflect the foundation surface that landed.
- `docs/QUALITY_SCORE.md` — permissions row moves from `D` to `C`;
  test framework + bench harness rows record the new `oran-permission`
  bucket (8 / 41 assertions; precedence-walk vs. find_if).
- `docs/design-docs/permissions-and-hooks.md` — added an "Engine
  status (2026-05-16)" note describing the foundation slice and what
  is still deferred (re2, capabilities, HMAC approvals, audit, config
  wiring).
- `docs/releases/feature-release-notes.md` — added the
  `permission-foundation` row with the new API surface, what is still
  downstream, and the bench numbers.
- `bench/permission/README.md` — new bench-bucket README mirroring the
  pattern used by other libraries.

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
  - `tests/permission/test_rule_set.cpp`: 8 cases / 41 assertions —
    literal + wildcard match, deny-wins, allow-first, ask precedence,
    mode default verdicts (all four), empty rule set, indexed reason.
- Bench impact:
  - `bench/permission/scenarios/rule_set.cpp` registers
    `permission.rule_set_evaluate` (~156 ns) vs.
    `permission.linear_find_if` (~83 ns) per evaluation of the same
    16-rule fixture.
- Compile-budget delta:
  - One small public header (stdlib-only: `<cstdint>`, `<format>`,
    `<string>`, `<string_view>`, `<vector>`). One new implementation
    TU. The new library stays under the
    `docs/design-docs/module-boundaries.md` per-TU budget for the
    permission/tool/hook/skill category.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none. Future slices already enumerated in the
  product spec (re2 input regex, capability gating, HMAC approvals,
  audit logging, config wiring).
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-05`
