## [2026-05-20 18:30] | Task: `file.search` regex opt-in (slice 24)

### Execution Context

- Agent: `Claude Code`
- Base model: `Claude Opus 4.7`
- Runtime: `Claude Code in local repository checkout`
- Linked plan: none — single-session slice that fits the
  tech-debt entry's "~50 LoC implementation + ~50 LoC tests + bench A/B"
  envelope and matches `PLANS_GUIDE.md` "When NOT To Create A Plan".
  The three other candidates listed in `STATUS.md` after slice 23
  remained blocked: the Anthropic Messages adapter is multi-slice and
  needs an exec plan ahead of code; blocking hook semantics with veto
  and `tool_dispatched` / `tool_error` publish both have their first
  consumer in the not-yet-existing `oran-agent`. `file.search` regex
  was the only tracked tech-debt that shipped user-visible value today
  without waiting on `oran-agent`.

### User Query

> 深度项目架构，了解当前项目实现进度, 继续推进项目代码实现.
> 实现完成需要 commit 和 push. ultrathink.
>
> (Deep project architecture, understand current project implementation
> progress, continue advancing the project code implementation.
> Implementation completion requires commit and push. Ultrathink.)

### Changes Overview

- **`file.search` input schema and tool description.** The JSON schema
  advertised on `core::ToolDef::input_schema_json` grows a new optional
  `"regex": {"type": "boolean"}` property; the human-readable
  description on the tool def explains the new path and the
  invalid-pattern error shape. `additionalProperties: false` remains —
  unknown keys still reject.
- **Per-call `LineMatcher`.** A small POD type in the file's anonymous
  namespace holds either a `std::string_view literal` (slice-20 path)
  or an engaged `std::optional<permission::InputPattern> regex`. Its
  `matches(std::string_view)` returns `regex ? regex->matches(line) :
  line.contains(literal)`. `scan_text` now takes a
  `const LineMatcher&` instead of `std::string_view pattern`, so the
  hot per-line loop has one virtual-less branch on the optional's
  engagement.
- **`build_matcher(opts)`.** The new helper is called once per
  `walk_and_scan`. When `opts.regex == false` it returns a literal
  matcher pointing into `opts.pattern`. When `opts.regex == true` it
  invokes `permission::InputPattern::compile(opts.pattern)`; on
  failure it returns an `Error::invalid_argument("file.search: invalid
  regex")` carrying `pattern=<source>` plus every context entry from
  the underlying compile error (notably `regex_error=<re2 message>`).
  On success it constructs the matcher with the engaged
  `InputPattern`. The compile happens once per call, ahead of the
  walk, so the directory-traversal case does not re-compile per file.
- **Reuse of `permission::InputPattern`.** The class already
  forward-declares `re2::RE2` (rule C6) so the file-search TU stays
  off `<re2/re2.h>`; the destructor lives in `oran-permission`'s
  source. No new package or include-graph dependency lands on
  `oran-tool` — re2 stays a private package of `oran-permission`,
  and `oran-tool` only sees the public `InputPattern` surface.
- **Slice-version bump.** `kVersion` 23 → 24. `xmake run orangutan
  --help` reports `orangutan v2.0.0-slice24`.
- **Tests.** `tests/tool/test_registry.cpp` grows by five
  `[unit][tool][file_search][regex]` cases plus one new entry in the
  existing malformed-input table:
  - `regex=true` happy on a single file with `error: \d+` —
    matches digit lines that would not match as literal.
  - `regex=true` happy on a recursive walk with `^TODO\([a-z]+\)` —
    proves the matcher is preserved across the directory iterator and
    `^` anchors per-line under partial-match semantics.
  - `regex=true` with `(unclosed` returns `invalid_argument` carrying
    `regex_error` and `pattern=(unclosed`.
  - `regex=true` honors `max_matches` and produces the same
    `(truncated; matches capped at N)` summary as the literal path.
  - `regex=false` explicit still treats `\d+` as a literal substring
    (a regression net for the default path).
  - The 9-shot malformed-input table grew to 10 (added `regex=` a
    non-boolean) — `sink.events().size() == 10` per call, all
    `outcome=allow` (every rejected call still passed the permission
    gate).

  `test-tool` grows to **63 cases / 516 assertions** (+5 cases / +39
  assertions). All ten test buckets stay green.
- **Bench.** `bench/tool/scenarios/file_search.cpp` adds the
  `file_search.literal_match_1kib` (~9.2 µs) vs.
  `file_search.regex_match_1kib` (~18.6 µs) A/B over the same 1 KiB
  seed file (32 lines × 32 bytes, one match in the middle). The
  ~9.4 µs delta pins the per-call re2 compile + 32× PartialMatch cost
  the agent loop pays when it opts into `"regex": true`. The existing
  `single_file_one_match` / `recursive_dir_many_matches` scenarios are
  unchanged.

### Design Intent

**Why a `LineMatcher` POD vs. inlining the branch in `scan_text`.**
`scan_text` is the hot loop. Threading a `const LineMatcher&` keeps
the branch predictable and lets the regex compile happen once in
`walk_and_scan` instead of being re-evaluated per file or per line.
The POD has two members (a `string_view` + an `optional<InputPattern>`)
so the engaged check is a single tag read; on the literal path
`InputPattern` is never constructed and re2 never touched. A
hand-rolled enum tag would have been equivalent shape but loses the
ownership of the compiled regex; `std::optional` is the right
container because `InputPattern` is move-only with a noexcept move.

**Why reuse `permission::InputPattern` instead of inlining re2.**
The class already encapsulates exactly the contract we need —
"compile a pattern once at runtime, partial-match it later, surface
a clean compile error with the re2 message attached". Adding re2 as
a direct dependency of `oran-tool` would have widened the include
graph (re2 transitively pulls abseil) and obligated us to duplicate
the quiet-options setup, the move-only wrapping, and the
`regex_error`-context idiom. The naming concern (`InputPattern` is
designed for rule input patterns) is real but minor — the class
itself is a generic re2 wrapper, and if a third consumer lands we
can rename without changing semantics. The cost is a single
`#include <oran/permission/input_pattern.hpp>` in `file_search.cpp`,
and the `oran-tool` xmake target gains nothing because
`oran-permission` is already a dependency.

**Why `regex_error` lives in the error context, not a typed accessor.**
Same pattern as slice-23's `signum`, slice-21's `decision_reason`,
slice-17's `reason` — the `Error::context()` `(key, value)` pair set
is the cross-library extension point. Adding a typed accessor on
`Error` for one re2-specific field would pollute every non-regex
error site with the field. Callers that want the message do
`std::ranges::find` over `error.context()`; the test for it is the
canonical example.

**Why the rule `Rule::input_pattern` and the tool's `file.search`
regex share `InputPattern` instead of `permission::Pattern` /
`tool::Pattern`.** Both compile a re2 expression at runtime and
match it later via `PartialMatch`. The lifetime ownership is the
same (move-only, holds a `unique_ptr<re2::RE2>`). The error shape
is the same (`Error::invalid_argument("invalid regex").with(
"regex_error", ...)`). Splitting into two classes would add a
maintenance burden without exposing any new axis. If a future
re2 wrapper gains capture-group inspection or substitution
support, the right move is to extend `InputPattern` in place
(probably renamed at that point) — not to fork a parallel hierarchy.

**Why a separate `build_matcher` function vs. inlining the compile
in `walk_and_scan`.** The directory-walk case has three structural
exit paths (regular file, directory, neither) and threading the
matcher construction inline would put the regex-compile branch
under the `is_regular_file` / `is_directory` conditional. Hoisting
it to a helper that runs first makes the failure mode legible —
"invalid regex" returns before any filesystem call — and matches
the slice-20 pattern (`parse_input` runs first, returns
`invalid_argument`, then the walk starts).

**Why an early-exit on the self-move-assignment bug in the first
draft.** The first version of `build_matcher` wrote
`err = std::move(err).with(key, value)` inside the context-copying
loop. `std::move(err).with(...)` returns `Error&&` aliasing `err`
itself; the subsequent assignment is self-move-assignment which
`std::string` member of `Error` does not guarantee preserves
content — the `message_` string came out empty under GCC 16.1, so
the `result.error().message().contains("invalid regex")` test
asserted on `""`. The fix is to use the lvalue `.with(...)`
overload, which mutates in place and returns `Error&`. This is
the kind of bug that motivated the "prefer in-place over
chained-move when both work" idiom; the lvalue overload is the
right answer when the loop has the lvalue in hand. Worth flagging
because the rvalue chain pattern reads naturally and tooling does
not warn — only the test caught it.

### Files Modified

- `src/oran-tool/file_search.cpp` — schema string grows
  `"regex":{"type":"boolean"}`; new `LineMatcher` POD;
  `SearchOptions::regex` field; `parse_input` parses the new field
  with a non-boolean reject; `scan_text` takes
  `const LineMatcher&`; new `build_matcher` helper compiles the
  regex once and wraps compile errors as `file.search: invalid
  regex`; `walk_and_scan` calls `build_matcher` ahead of the
  filesystem walk; the tool description updated to advertise the
  opt-in. `#include <oran/permission/input_pattern.hpp>` and
  `<optional>` added; file-level comment rewritten for slice 24.
- `tests/tool/test_registry.cpp` — five new `[unit][tool][file_search][regex]`
  cases (~115 LoC total) and a one-line addition to the malformed-input
  table (`wrong_regex` + `== 10` sink-event count).
- `bench/tool/scenarios/file_search.cpp` — file-level comment
  expanded to describe the new A/B; new `make_seed_1kib()` helper
  builds a 1 KiB seed with one match in the middle; the `TempTree`
  fixture now writes `seed_1kib.txt`; two new `bench.run` calls for
  `literal_match_1kib` and `regex_match_1kib`.
- `bench/tool/README.md` — new bullet describing the literal-vs-regex
  A/B.
- `src/oran-bootstrap/bootstrap.cpp` — `kVersion` 23 → 24.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 24, history pointer, `oran-tool` library
  surfaces row (63 cases / 516 assertions), refreshed `Next intended
  slice` to drop the now-landed regex item, removed the closed
  tech-debt row.
- `docs/QUALITY_SCORE.md` — Test framework row refreshed with the
  new `oran-tool` counts; Bench harness row extended with the new
  `literal_match_1kib` / `regex_match_1kib` A/B; Tool registry row
  rewritten to describe the new `regex` input field, the
  `permission::InputPattern`-backed code path, and the
  `invalid_argument` + `regex_error` context shape; Tool registry
  "Next Step" column trimmed (regex no longer pending).
- `docs/ARCHITECTURE.md` — slice-status preamble lists slice 24
  alongside the prior slices; `oran-tool` inventory row updated to
  mention the `regex?: bool` input and the slice-24 path.
- `docs/exec-plans/tech-debt-tracker.md` — `file.search` regex row
  removed (closed by this slice). The "ripgrep-class optimisations"
  row stays — it depends on a real `oran-agent` workload
  measurement and is not closed.
- `docs/releases/feature-release-notes.md` — new top row
  `oran-tool-file-search-regex`.
- `docs/histories/2026-05/20260520-1830-file-search-regex.md` —
  this file.

### Validation

- Commands run:
  - `xmake build oran-tool` — clean (~8.5 s).
  - `xmake build test-tool` — clean (~33 s).
  - `./build/linux/x86_64/release/test-tool` — 63 cases / 516
    assertions, all green.
  - `./build/linux/x86_64/release/test-tool "[regex]"` — 5 cases /
    36 assertions, all green.
  - `xmake test` — all 10 buckets green (test-async / bootstrap /
    cli / config / core / hook / io / permission / storage / tool).
  - `xmake build bench-tool && xmake run bench-tool` — clean;
    measured `file_search.literal_match_1kib ~9,227 ns` vs.
    `file_search.regex_match_1kib ~18,645 ns` (~9.4 µs A/B delta).
    Existing scenarios within noise: `file_search.single_file_one_match`
    ~8,438 ns, `file_search.recursive_dir_many_matches` ~29,636 ns;
    `registry.dispatch_allow` ~2,177 ns; `dispatch_allow_with_two_sinks`
    ~2,859 ns.
- Tests added/changed: 5 new tool-bucket cases (+39 assertions); one
  existing case extended (+1 malformed-input row).
- Bench impact: existing scenarios unchanged within noise. Two new
  scenarios baselined above.
- Compile-budget delta: one extra public include in
  `src/oran-tool/file_search.cpp` (`<oran/permission/input_pattern.hpp>`).
  No new packages or transitive headers reach `oran-tool` —
  `InputPattern` forward-declares `re2::RE2`. `bench-tool` and
  `test-tool` were already on `oran-permission`'s include path.
  No PCH change.

### Follow-ups

- Issues to file: none.
- Tech-debt entries filed: none. The remaining `file.search` tech-debt
  row (ripgrep-class optimisations) is unchanged — it is still gated
  on a real `oran-agent` workload measurement.
- Linked release note: 2026-05-20 `oran-tool-file-search-regex` row in
  `docs/releases/feature-release-notes.md`.
- Cross-references for future agents: the `LineMatcher` shape is a
  small template for the next built-in that needs "literal vs. compiled
  pattern" — the same POD-with-optional-engaged-payload pattern works
  for any tool that wants a "fast default + opt-in heavy" code path.
  If a third consumer of `permission::InputPattern` lands, consider
  renaming the class to `core::RuntimeRegex` and moving it into
  `oran-core` so the dependency direction stays clean (currently
  `oran-tool` reaches into `oran-permission` for what is really a
  re2 wrapper).
