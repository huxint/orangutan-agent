## [2026-05-17 01:55] | Task: bootstrap --explain-rules + RuleSet::rules() accessor

### Execution Context

- Agent: `Claude Code`
- Base model: `Claude Opus 4.7`
- Runtime: `Claude Code, orangutan-refactor`
- Linked plan: none — third commit of the `0008-permissions.md`
  criterion 4 push that slices 8 and 9 opened.

### User Query

> 详细了解项目目标，查看当前项目真实进度, 推进项目代码的实现.
> 你这一次推进应该是能够实现 3 个commit左右. 不要盲目实现, 需要凭借客观事实,
> 良好的代码工程和查阅网上资料进行实现 ultrathink.

### Changes Overview

- Areas: `src/oran-bootstrap/bootstrap.cpp`,
  `include/oran/permission/rule_set.hpp`,
  `tests/permission/test_rule_set.cpp`, `xmake/targets.lua`,
  `docs/releases/feature-release-notes.md`, `docs/STATUS.md`,
  `docs/QUALITY_SCORE.md`.
- Key actions:
  - `oran-bootstrap` gains a `--explain-rules` flag. When set,
    `bootstrap::run` calls `permission::materialize(
    Mode::default_, config.permissions(),
    config::PermissionsConfig{})` after loading config and prints
    each rule (index, verdict, tool pattern, optional capability,
    optional `input=~"<pattern>"`) to stdout, then exits cleanly
    with `Result<int>{0}`. The "default" mode + empty per-agent
    overlay is a deliberately diagnostic-friendly shape; a
    future slice can plumb the operator's selected mode and
    agent. End-to-end smoke against `config.example.json`
    produces 17 rules (9 defaults + 8 from config, including
    the two `input_pattern`-backed deny rules added in slice 9).
    The legacy "rule explain CLI subcommand" risk noted in
    `docs/product-specs/0008-permissions.md` is now mitigated.
  - `oran-bootstrap` declares a new build-time dep on
    `oran-permission` (legal — bootstrap sits above permission
    in the layering). The `xmake/targets.lua` line for
    `oran_lib("bootstrap", …)` adds `"oran-permission"` to
    the dep list.
  - `permission::RuleSet` exposes
    `rules() -> std::span<const Rule>` (insertion order;
    `noexcept`), so diagnostics can iterate the materialized
    rule set without re-running `evaluate`. The accessor is
    decoupled from the precedence-walk loop inside `evaluate`
    so the hot path stays pointer-following over the same
    `rules_` member without an extra indirection.
  - `tests/permission/test_rule_set.cpp` adds one new test
    case for the accessor (insertion order + capability
    visibility). Permission tests grew to 42 cases / 172
    assertions.
  - `src/oran-bootstrap/bootstrap.cpp` slice tag bumped 9 → 10.
  - `print_usage` mentions `--explain-rules` so `--help`
    documents it for users.
  - Docs:
    - `docs/releases/feature-release-notes.md` gains a new
      `2026-05-17 permission-explain-rules` row plus
      back-dated rows for the two earlier slices that
      together closed `0008-permissions.md` criterion 4.
      The cumulative release-notes table now reflects the
      slice 8 → 9 → 10 progression.
    - `docs/STATUS.md` slice 10, new history pointer,
      refreshed permission test counts.
    - `docs/QUALITY_SCORE.md` test framework + bootstrap
      rows refreshed; bootstrap "Why" string now mentions
      `--explain-rules`.

### Design Intent

`--explain-rules` is a small but real win: it closes the
diagnostic-tool gap mentioned as a mitigating control in the
0008 product spec's Risks section ("`Misconfigured rules
silently broaden permissions — schema validation + a 'rule
explain' CLI subcommand mitigate.`"). The cost is minimal —
one small printer plus one accessor — and the value is
immediate: operators can sanity-check a `permissions` block
without writing a script.

Choosing `Mode::default_` for the explain output rather than
prompting for a mode keeps the slice scope small. A future
slice can add `--explain-rules-mode=<mode>` and an
`--explain-rules-agent=<name>` once `oran-bootstrap` owns
mode selection. For today, "default mode, no agent picked"
matches what 99 % of operators will want to see.

The `RuleSet::rules()` accessor is intentionally read-only:
exposing a mutable handle would invite ad-hoc rule mutation
from outside the constructor + `materialize` path, which is
exactly the kind of escape hatch `oran-permission`'s
encapsulation has been protecting. A `std::span<const Rule>`
is the right shape for diagnostic iteration without giving
callers a way to corrupt internal state.

### Files Modified

- `src/oran-bootstrap/bootstrap.cpp` — `--explain-rules`
  flag + `print_materialized_rules` helper + slice tag bump
  + new `<oran/permission.hpp>` and `<oran/core/enum_names.hpp>`
  includes.
- `include/oran/permission/rule_set.hpp` —
  `rules() -> std::span<const Rule>` accessor with rationale
  comment.
- `tests/permission/test_rule_set.cpp` — one new test case
  for the accessor (3 assertions over 3 rule positions).
- `xmake/targets.lua` — `oran-bootstrap` deps include
  `oran-permission`.
- `docs/releases/feature-release-notes.md` — three new
  release-notes rows for the slice 8 + 9 + 10 trio.
- `docs/STATUS.md` — slice 10, history pointer, surface
  counts.
- `docs/QUALITY_SCORE.md` — test framework + bootstrap rows.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/releases/feature-release-notes.md` — new rows for
  the user-visible `--explain-rules` flag plus the two
  preceding code-only slices that share the same release.
- `docs/STATUS.md` — slice bump, history pointer.
- `docs/QUALITY_SCORE.md` — multiple rows.
- This history entry —
  `docs/histories/2026-05/20260517-0155-bootstrap-explain-rules.md`.

### Validation

- Commands run:
  - `xmake build oran-bootstrap` (clean after adding the
    `oran-permission` dep).
  - `xmake build test-bootstrap && ./test-bootstrap` —
    8 cases / 34 assertions, all green (existing tests
    unaffected).
  - `xmake build test-permission && ./test-permission` —
    42 cases / 172 assertions (new `rules()` accessor
    coverage).
  - `xmake build orangutan && ./orangutan --explain-rules`
    on the built-in defaults: prints 9 rules.
    `./orangutan --config config.example.json --explain-rules`:
    prints 17 rules including the two `input_pattern`-backed
    `deny` rules from the example.
  - `scripts/check-docs-sync.sh` (clean).
- Tests added/changed: 1 new permission test case (3
  assertions).
- Bench impact: none; `--explain-rules` is a one-shot
  diagnostic that runs at most once per invocation.
- Compile-budget delta: `oran-bootstrap` now pulls
  `<oran/permission.hpp>` through `<oran/permission/input_pattern.hpp>`
  in one TU; re2 still stays behind a forward declaration in
  the public header. The bootstrap TU's transitive header
  set grew by a handful of stdlib + repo headers but no new
  third-party headers.

### Follow-ups

- Issues to file: none.
- Tech-debt entry: none.
- Linked release note: `permission-explain-rules` row added
  to `feature-release-notes.md`.
- Plumb mode selection + per-agent picking into
  `--explain-rules` once `oran-bootstrap` owns either.
- HMAC-signed approval prompts (`0008-permissions.md`
  criterion 5).
- Hoist `InputPattern` to `core::RuntimeRegex` when
  `oran-log`'s redaction patterns need re2.
