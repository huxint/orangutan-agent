## [2026-05-17 01:36] | Task: oran-permission InputPattern (re2)

### Execution Context

- Agent: `Claude Code`
- Base model: `Claude Opus 4.7`
- Runtime: `Claude Code, orangutan-refactor`
- Linked plan: none — `0008-permissions.md` criterion 4 close-out
  was already named as the next intended slice in `STATUS.md`.

### User Query

> 详细了解项目目标，查看当前项目真实进度, 推进项目代码的实现.
> 你这一次推进应该是能够实现 3 个commit左右. 不要盲目实现, 需要凭借客观事实,
> 良好的代码工程和查阅网上资料进行实现 ultrathink.

### Changes Overview

- Areas: `xmake/` (new dependency), `include/oran/permission/`,
  `src/oran-permission/`, `tests/permission/`, `bench/permission/`,
  `docs/rules/libraries.md`, `docs/design-docs/permissions-and-hooks.md`,
  `docs/product-specs/0008-permissions.md`, `docs/QUALITY_SCORE.md`,
  `docs/STATUS.md`, `bench/permission/README.md`,
  `src/oran-bootstrap/bootstrap.cpp` (slice tag bump).
- Key actions:
  - Added `re2 2025.11.05` to `xmake/packages.lua` (and the
    transitive `abseil 20260107.0` lock entry); `oran-permission`
    now declares `re2` as a private package. `re2` was already on
    the approval list in `docs/rules/libraries.md`; only the
    version bumped (the doc previously named `2024.07.02` as a
    placeholder).
  - Introduced `permission::InputPattern`, a move-only wrapper that
    owns a `std::unique_ptr<re2::RE2>`. The public header forward-
    declares `re2::RE2` only — full `<re2/re2.h>` lives in the
    `.cpp` so the C6 critical-rule ban on heavyweight third-party
    includes in `include/oran/` stays honest. The factory
    `InputPattern::compile(std::string)` returns
    `core::Result<InputPattern>`; failures carry the re2 error
    message as the `regex_error` context entry on
    `Error::invalid_argument`. Compilation is quiet
    (`re2::RE2::Options::set_log_errors(false)`) so invalid
    patterns never leak to stderr — config loaders are expected
    to surface the typed error themselves.
  - `permission::Rule` grew an optional `input_pattern` field.
    `Rule` is now move-only (`std::optional<InputPattern>` is
    move-only because re2's `RE2` is neither copyable nor
    movable). `friend bool operator==(const Rule&, const Rule&) =
    default;` stays valid because `InputPattern` defines equality
    on its source pattern string (no calls into re2's internals).
  - `RuleSet::evaluate` gained a four-argument overload
    `(tool_name, input, required_capabilities, mode)`. The
    existing two- and three-argument overloads forward to it
    with `input == ""`; rules with an `input_pattern` that does
    not accept the empty string therefore never fire on the
    no-input path (a documented, honest semantics). The reason
    formatter prints `input=~"<pattern>"` when the firing rule
    had an `input_pattern`, alongside the existing
    `capability=<name>` annotation.
  - `tests/permission/test_input_pattern.cpp` covers the re2
    compile success / failure paths, partial-match vs anchored
    semantics, move-construction, source-string equality, the
    Rule + input_pattern firing and missing paths, the
    no-input-evaluate skip rule, and the capability + input
    composition on a single rule. Permission tests now total
    39 cases / 149 assertions (was 30 / 114).
  - `bench/permission/scenarios/input_pattern.cpp` registers
    three A-vs-B scenarios:
    `permission.input_pattern_match` (re2 success path),
    `permission.input_pattern_miss` (re2 failure + fallback
    path), and `permission.no_input_pattern` (the same rule
    shape without an `input_pattern`, anchoring the cost
    *removed* by skipping the re2 hop). Initial numbers on this
    machine: 192 ns / 63 ns / 100 ns per `evaluate`.
  - `bench/permission/scenarios/rule_set.cpp` switched to
    `std::move` when populating the two fixture rule sets — the
    move-only `Rule` change above forced this; the bench
    semantics are unchanged.
  - Docs:
    - `docs/design-docs/permissions-and-hooks.md` engine-status
      block updated to dated 2026-05-17 and now describes
      `InputPattern` + the four-argument `evaluate` overload.
    - `docs/product-specs/0008-permissions.md` criterion 4
      status updated to note runtime regex landed 2026-05-17
      (config-side `input_pattern` parsing remains the matching
      `oran-config` slice).
    - `docs/QUALITY_SCORE.md` permissions row repointed at the
      new test counts, bench scenarios, and next step.
    - `docs/rules/libraries.md` re2 version bumped 2024.07.02
      → 2025.11.05 to match `xmake/packages.lua`. `check-docs-sync`
      now passes.
    - `bench/permission/README.md` gains a row for
      `scenarios/input_pattern.cpp`.
  - `src/oran-bootstrap/bootstrap.cpp` slice tag bumped 7 → 8.

### Design Intent

`0008-permissions.md` criterion 4 splits naturally in two: a
runtime engine that owns the regex match semantics, and a
config-loading path that parses, validates, and reports errors
on operator-supplied patterns. This slice owns the engine half.
Putting the config-side parsing in the next slice keeps each
diff small, keeps the `oran-config` test bucket honest about
what *its* tests are validating, and lets the re2 dependency
land in `oran-permission` (its primary consumer per
`docs/rules/libraries.md`) without dragging an `oran-config`
private dep before the wiring exists.

`re2` was chosen over `std::regex`, PCRE2, and `ctre` in
`docs/rules/libraries.md`; this slice just consumes that
decision. The library's value here is the linear-time guarantee
on adversarial input — config-driven patterns *will* be
malformed sometimes (operator typos, copy-paste errors); a
`std::regex` engine catastrophic-backtracks on the wrong shape;
`re2` does not.

Match semantics are intentionally `PartialMatch`, not `FullMatch`.
The design-doc YAML examples (`"shell.exec(rm:*)"`, `"shell.exec(git push *)"`)
read as substring filters; operators reaching for an anchored
match can prefix with `^` and suffix with `$` (a one-keystroke
overhead in the common case, much less awkward than the
inverse).

`InputPattern` is move-only because `re2::RE2` itself is
non-copyable, non-movable. Rule's resulting move-only status
forced the bench fixtures to switch from copy-into-vector to
move-into-`RuleSet`. The change is purely mechanical at the
fixture level; the *evaluator* surface is untouched.

The new `Decision::reason` formatter prints `input=~"<pattern>"`
specifically because audit-log readers need to know *why* a
deny fired. Without the pattern in the reason, an operator
inspecting "rule #3 fired" would have to look up the rule list
by index — fine in tests, fragile in production.

### Files Modified

- `xmake/packages.lua` — `re2 2025.11.05`.
- `xmake/targets.lua` — `oran-permission` private dep on `re2`.
- `xmake-requires.lock` — abseil + re2 lock entries.
- `include/oran/permission/input_pattern.hpp` — new file.
- `include/oran/permission/rule_set.hpp` — `Rule::input_pattern`,
  four-argument `evaluate` overload, updated commentary.
- `include/oran/permission.hpp` — umbrella include for the new
  header.
- `src/oran-permission/input_pattern.cpp` — new file.
- `src/oran-permission/rule_set.cpp` — input-pattern match in
  the precedence walk; reason formatter prints the pattern.
- `src/oran-permission/defaults.cpp` — explicit
  `.input_pattern = std::nullopt` on each rule construction.
- `src/oran-permission/materialize.cpp` — same, on the
  config-side rule conversion.
- `src/oran-bootstrap/bootstrap.cpp` — slice tag bump.
- `tests/permission/test_input_pattern.cpp` — new file (8
  test cases / 35 assertions).
- `bench/permission/scenarios/input_pattern.cpp` — new file.
- `bench/permission/scenarios/rule_set.cpp` — move-into-`RuleSet`
  to satisfy the move-only `Rule`.
- `bench/permission/main.cpp` — register the new scenario block.
- `bench/permission/README.md` — new row for the input_pattern
  scenarios.
- `docs/rules/libraries.md` — re2 version bumped.
- `docs/design-docs/permissions-and-hooks.md` — engine-status
  block updated.
- `docs/product-specs/0008-permissions.md` — criterion 4 status
  updated.
- `docs/QUALITY_SCORE.md` — permissions and bench rows.
- `docs/STATUS.md` — slice 8, last-completed-history pointer,
  permission test counts, next-intended-slice line.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/rules/libraries.md` — re2 version bumped to 2025.11.05.
- `docs/design-docs/permissions-and-hooks.md` — engine-status
  block now mentions `InputPattern` + the four-argument
  evaluate overload.
- `docs/product-specs/0008-permissions.md` — criterion 4 status.
- `docs/QUALITY_SCORE.md` — permissions row + bench row.
- `docs/STATUS.md` — slice bump, history pointer, library
  surface counts, next-intended-slice candidate.
- `bench/permission/README.md` — new scenarios row.
- This history entry —
  `docs/histories/2026-05/20260517-0136-oran-permission-input-pattern.md`.

### Validation

- Commands run:
  - `xmake build oran-permission` (clean).
  - `xmake build test-permission && ./test-permission` —
    39 cases / 149 assertions, all green.
  - `xmake build bench-permission && ./bench-permission` —
    new `input_pattern` block runs; no regressions on the
    existing `rule_set` / `defaults` / `materialize` blocks.
  - `xmake build test-bootstrap && ./test-bootstrap` —
    8 cases / 34 assertions, all green (slice tag bump verified).
  - `scripts/check-includes.sh` (clean — public header forward-
    declares `re2::RE2` rather than including `<re2/re2.h>`).
  - `scripts/check-docs-sync.sh` (clean after libraries.md bump).
- Tests added/changed: `test_input_pattern.cpp` (new), plus
  no-op `.input_pattern = std::nullopt` updates in two existing
  `.cpp` files (no behavior change).
- Bench impact: new `bench-permission/input_pattern` block with
  three scenarios; existing scenarios unchanged in shape (the
  `rule_set.cpp` fixture-build switched from copy to move but
  the work measured is identical).
- Compile-budget delta: re2 adds CMake build cost the first
  time `xmake f` resolves it; the compiled `oran-permission`
  TU set grew by one file (`input_pattern.cpp`) and the
  archive grew accordingly. No public-header include cost
  added (re2 stays behind a forward declaration).

### Follow-ups

- Issues to file: none.
- Tech-debt entry: none.
- Linked release note: deferred — slice 8 + slice 9 close
  criterion 4 together; the release note can land with slice 9.
- Wire `input_pattern` through `oran-config` next so operators
  can author regex from `config.permissions` JSON and get
  line-numbered errors at load time (the second half of
  `0008-permissions.md` criterion 4).
