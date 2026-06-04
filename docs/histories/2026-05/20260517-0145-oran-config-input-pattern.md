## [2026-05-17 01:45] | Task: wire input_pattern through oran-config (close 0008 criterion 4)

### Execution Context

- Agent: `Claude Code`
- Base model: `Claude Opus 4.7`
- Runtime: `Claude Code in local repository checkout`
- Linked plan: none — slice 9 of the
  `0008-permissions.md` criterion 4 push that slice 8 opened.

### User Query

> 详细了解项目目标，查看当前项目真实进度, 推进项目代码的实现.
> 你这一次推进应该是能够实现 3 个commit左右. 不要盲目实现, 需要凭借客观事实,
> 良好的代码工程和查阅网上资料进行实现 ultrathink.

### Changes Overview

- Areas: `include/oran/config/config.hpp`,
  `src/oran-config/config.cpp`, `include/oran/permission/materialize.hpp`,
  `src/oran-permission/materialize.cpp`, `tests/config/test_config.cpp`,
  `tests/permission/test_materialize.cpp`,
  `bench/config/scenarios/permissions.cpp`,
  `bench/config/README.md`,
  `bench/permission/scenarios/materialize.cpp`,
  `config.example.json`, `xmake/targets.lua`, `xmake/packages.lua`,
  `xmake-requires.lock` (unchanged, lock already had re2),
  `docs/rules/libraries.md`, `docs/STATUS.md`,
  `docs/QUALITY_SCORE.md`,
  `docs/design-docs/permissions-and-hooks.md`,
  `docs/product-specs/0008-permissions.md`,
  `src/oran-bootstrap/bootstrap.cpp` (slice tag bump).
- Key actions:
  - `config::PermissionRuleConfig` grew an
    `std::optional<std::string> input_pattern` field. The parser
    accepts an optional `input_pattern` JSON string on each
    permission rule (under `permissions.{allow,deny,ask}` and the
    `agents.<name>.permissions` overlay alike) and validates it
    at load time by compiling re2 once with logging disabled.
    Invalid patterns surface as `Error::config` with the JSON
    path attached (e.g. `$.permissions.deny[0].input_pattern`)
    and the re2 error message recorded under the `regex_error`
    context key — closing criterion 4's "invalid patterns at
    load time are reported" guarantee.
  - `permission::materialize` now returns
    `core::Result<RuleSet>` and, when assembling each runtime
    `Rule`, recompiles the validated `input_pattern` source via
    `permission::InputPattern::compile`. A compile failure at
    that point is theoretical (the source already survived a
    re2 compile during config load), but the path remains
    defensive — we propagate `Error::invalid_argument` rather
    than silently drop the rule.
  - `oran-config` gains a private dependency on `re2` (added
    in `xmake/targets.lua` and `docs/rules/libraries.md`'s
    "used by" column). `xmake/targets.lua`'s `oran_lib` helper
    switched from `add_packages(table.unpack(pkgs), opts)` to
    a `for/ipairs` loop because the table.unpack form was
    silently dropping the second package — verified via
    `xmake show -t oran-config`. The fix preserves the
    single-package case in every other library.
  - `tests/config/test_config.cpp` gained two new test cases:
    `Config::parse extracts input_pattern on permission rules`
    (4 assertions) and `Config::parse rejects malformed
    input_pattern at load time` (8 assertions across three
    sections covering invalid regex with path + re2 error
    context, empty string rejection, and non-string type
    rejection). Test counts grew to 14 cases / 132 assertions
    (was 12 / 120).
  - `tests/permission/test_materialize.cpp` gained two new test
    cases for the `Result<RuleSet>` return shape: one that
    compiles a valid config-side `input_pattern` into a
    runtime regex and exercises evaluate, one that bypasses
    the config-side validator with a malformed pattern and
    confirms materialize surfaces `Error::invalid_argument`.
    All existing test-cases were rewritten to call the new
    `require_materialized(...)` helper or unwrap the result
    explicitly. Test counts grew to 41 cases / 165 assertions
    (was 39 / 149).
  - `bench/config/scenarios/permissions.cpp` gained a third
    A-vs-B scenario `config.parse_permissions_with_input_patterns`
    that parses a 14-rule block with four `deny` rules each
    carrying a `^...$` style `input_pattern`. Initial numbers
    on this machine: empty 869 ns; typed-no-regex 18.7 µs;
    typed-with-4-patterns 20.0 µs — so each re2 compile at
    load adds ≈ 300 ns.
  - `bench/permission/scenarios/materialize.cpp` updated to
    unwrap the new `Result<RuleSet>` return.
  - `config.example.json` updated: the placeholder
    `shell.exec(rm:*)` deny was replaced with two
    `input_pattern`-backed rules (`^rm -rf` and `^git push`).
    The example permissions block grew 7 → 8 rules; the test
    that asserts the count was updated.
  - Docs:
    - `docs/design-docs/permissions-and-hooks.md` engine-status
      block extended with the config-side wiring + the
      `materialize` Result-shape change.
    - `docs/product-specs/0008-permissions.md` criterion 4
      flipped from "foundation" to "closed 2026-05-17" with
      the load-time validation contract spelled out.
    - `docs/QUALITY_SCORE.md` config + permissions + tests +
      bench rows refreshed.
    - `docs/rules/libraries.md` re2 "used by" column now
      lists `config, log, permission, tool`.
    - `docs/STATUS.md` slice 9, new history pointer, updated
      test counts, refreshed next-intended-slice line.
  - `src/oran-bootstrap/bootstrap.cpp` slice tag 8 → 9.

### Design Intent

`0008-permissions.md` criterion 4 splits naturally into the
matching engine (slice 8) and the config-loading path (this
slice). Putting the config-side validation in its own slice
keeps the dependency surface honest: `oran-config` now owns the
re2 compile that proves the operator-supplied string is a
syntactically valid pattern, and `oran-permission::materialize`
owns the recompile that produces the runtime matcher. We could
have stored a compiled `InputPattern` directly in the config
struct, but that would have:

  - Made `PermissionRuleConfig` move-only (re2's `RE2` is
    non-copyable, non-movable), breaking the test fixtures
    that rely on copy semantics.
  - Inverted the library dependency direction (`oran-config`
    would have had to depend on `oran-permission::InputPattern`
    while `oran-permission` already depends on `oran-config`).
  - Saved one re2 compile per rule (≈ 300 ns) — an immaterial
    win compared to the structural cost.

Re-compiling once at materialize time is the right
trade-off. The Result<RuleSet> return is defensive: in normal
operation the validator already accepted the source string, so
the recompile cannot fail; we surface the error path anyway so
a transient re2 failure does not silently drop a rule.

The xmake `oran_lib` change (loop over packages instead of
`add_packages(table.unpack(...), opts)`) was forced by a real
bug: `xmake show -t oran-config` proved that only the first
package was being attached when the table contained two
entries. The loop form is also slightly clearer for readers.

`config.example.json` switched away from the
`shell.exec(rm:*)` shell-glob shape because that shape was
never actually parsed — it lived in the design-doc as
aspirational future syntax. `input_pattern` replaces it with
real, parseable regex; reading the example now teaches
operators the intended approach.

### Files Modified

- `include/oran/config/config.hpp` — `PermissionRuleConfig::input_pattern`
  field with default `{}` initializer.
- `src/oran-config/config.cpp` — parse + validate
  `input_pattern`; private `<re2/re2.h>` include.
- `include/oran/permission/materialize.hpp` — `materialize`
  returns `Result<RuleSet>`; updated commentary.
- `src/oran-permission/materialize.cpp` — recompile
  `InputPattern` per rule; propagate compile failure.
- `tests/config/test_config.cpp` — two new test cases; one
  updated assertion (example permissions size 7 → 8).
- `tests/permission/test_materialize.cpp` — Result unwraps via
  the new `require_materialized` helper; two new
  input_pattern test cases.
- `bench/config/scenarios/permissions.cpp` — new
  `config.parse_permissions_with_input_patterns` scenario.
- `bench/permission/scenarios/materialize.cpp` — unwrap
  Result.
- `bench/config/README.md` — new scenarios row.
- `config.example.json` — replace `shell.exec(rm:*)` with
  two `input_pattern` deny rules.
- `xmake/targets.lua` — loop over packages; add `re2` to
  `oran-config` private deps.
- `xmake/packages.lua` — unchanged; `re2` already added in
  slice 8.
- `docs/rules/libraries.md` — re2 "used by" includes config.
- `docs/design-docs/permissions-and-hooks.md` — engine-status
  block.
- `docs/product-specs/0008-permissions.md` — criterion 4
  closed.
- `docs/QUALITY_SCORE.md` — permissions + config + tests +
  bench rows.
- `docs/STATUS.md` — slice 9, history pointer, library
  surface counts.
- `src/oran-bootstrap/bootstrap.cpp` — slice tag bump.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/rules/libraries.md` — re2 "used by" extended.
- `docs/design-docs/permissions-and-hooks.md` — engine-status
  block.
- `docs/product-specs/0008-permissions.md` — criterion 4
  status.
- `docs/QUALITY_SCORE.md` — multiple rows.
- `docs/STATUS.md` — slice bump, history pointer, surface
  counts.
- `bench/config/README.md` — new scenarios row.
- This history entry —
  `docs/histories/2026-05/20260517-0145-oran-config-input-pattern.md`.

### Validation

- Commands run:
  - `xmake build oran-config` (clean after the `oran_lib`
    package-loop fix).
  - `xmake build && xmake test` — all 8 buckets green:
    `oran-core 54/366`, `oran-async 8/38`, `oran-io 8/33`,
    `oran-storage 52/606`, `oran-config 14/132`,
    `oran-permission 41/165`, `oran-cli 5/30`,
    `oran-bootstrap 8/34`.
  - `xmake build bench-config && ./bench-config` — new
    `config.parse_permissions_with_input_patterns` scenario
    runs.
  - `xmake build bench-permission && ./bench-permission` —
    materialize scenarios still run cleanly after the
    Result<RuleSet> unwrap.
  - `scripts/check-includes.sh` (clean — config's re2
    include lives in `src/oran-config/config.cpp`, not in
    `include/oran/`).
  - `scripts/check-docs-sync.sh` (clean).
- Tests added/changed: 2 new config tests (12 assertions),
  2 new materialize tests (16 assertions). Existing
  materialize tests rewritten to handle the
  `Result<RuleSet>` return — no behavior change at the
  assertion level.
- Bench impact: new
  `config.parse_permissions_with_input_patterns` scenario;
  existing scenarios unchanged in shape. Each re2 compile at
  load costs ≈ 300 ns.
- Compile-budget delta: `oran-config` now consumes re2's
  headers in one TU (`config.cpp`). The public-header impact
  is zero — re2 stays behind a private include.

### Follow-ups

- Issues to file: none.
- Tech-debt entry: none.
- Linked release note: deferred — slice 8 + slice 9 close
  criterion 4 together; the release note can land with the
  bootstrap-wiring slice that ties materialize() into the
  startup path.
- Hoist `InputPattern` to `core::RuntimeRegex` once
  `oran-log`'s redaction patterns need re2 (per
  `docs/rules/libraries.md` "log, permission, config, tool").
- Land HMAC-signed approval prompts per criterion 5.
