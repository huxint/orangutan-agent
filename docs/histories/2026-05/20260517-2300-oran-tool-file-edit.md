## [2026-05-17 23:00] | Task: add `file.edit` built-in on top of slice-17 `tool::Registry`

### Execution Context

- Agent: `Claude Code`
- Base model: `Claude Opus 4.7`
- Runtime: `Claude Code in local repository checkout`
- Linked plan: none — single-session slice that fits the
  `Next intended slice` bullet in
  [`STATUS.md`](../../STATUS.md) ("the remaining two file built-ins
  (`file.edit`, `file.search`)"), matching `PLANS_GUIDE.md`
  "When NOT To Create A Plan".

### User Query

> 提交，直接合并为一个提交. 然后继续实现.
> (Commit, directly merge into one commit. Then continue
> implementing.)

The previous turn produced slices 17 (`tool::Registry` foundation +
`file.read`) and 18 (`file.write`) as a working tree on top of a
slice-16 HEAD. The user asked for the two slices to land as one
combined commit, then for the next coherent slice. Slice 19 picks
`file.edit` — the natural next file built-in and item #1 on the
`Next intended slice` candidate list — and ships it on top of the
combined slice-17/18 commit.

### Changes Overview

- **New built-in `file.edit`.** Registered via
  `tool::register_file_edit` and aggregated into
  `tool::register_builtins`. Capability `Capability::edit_file`.
  Schema: `{"path": <string>, "old_string": <string>,
  "new_string": <string>, "replace_all"?: bool (default false)}`,
  `additionalProperties:false`. Handler parses with `nlohmann::json`
  in its own TU, refuses empty `old_string` and identical
  `old_string == new_string` (both `invalid_argument`), reads the
  file via `io::read_text_file`, scans the contents for
  non-overlapping occurrences of `old_string`, refuses if there are
  zero matches (`not_found`) or more than one match with
  `replace_all=false` (`conflict` with the match count carried as
  context), rebuilds the contents in a single pass via
  `apply_replacements`, and writes back via `io::write_text_file`
  in `truncate` mode. On success: `Output{ text = "edited <path>:
  N replacement(s)" }` with correct singular/plural wording for
  `N=1` vs `N>1`.
- **`register_builtins` order.** `file.read` → `file.write` →
  `file.edit`; the slice's catalog test asserts size 3 and the
  insertion-order triple.
- **`tests/tool` extension.** 7 new cases / 66 new assertions
  cover:
  - `register_file_edit` advertises `Capability::edit_file` and the
    four-field schema.
  - Happy path single replacement on a unique match writes the new
    bytes and reports `1 replacement` (singular wording).
  - `replace_all=true` rewrites every match and reports
    `N replacements` (plural wording).
  - Non-unique `old_string` with `replace_all=false` returns
    `Error::conflict`, leaves the file intact, and carries
    `match_count` in the error context.
  - Missing-substring returns `Error::not_found` without touching
    the file.
  - Missing file propagates `Error::not_found` from `oran-io`
    unchanged.
  - An eight-shot malformed-input table that covers bad JSON,
    missing path, missing old_string, missing new_string,
    non-string path, non-bool replace_all, empty old_string, and
    `old_string == new_string` no-op rejection — plus asserts each
    malformed call still recorded one `allow` audit row, proving
    the permission decision is independent of payload validity.
- **`bench-tool` extension.** New scenario file
  `bench/tool/scenarios/file_edit.cpp` adds
  `file_edit.dispatch_unique_replace` vs.
  `file_edit.dispatch_replace_all_many` on a 1 KiB seed. Measured
  ~15.1 µs vs. ~16.1 µs (the ~1 µs A/B delta is 63 extra
  substitutions costing less than ~16 ns each over the shared
  read+write floor — well below the dispatch overhead at slice 17,
  confirming the substitution loop is not on the hot path the
  agent loop would need to optimise). The existing
  `registry.lookup` (~8 ns), `registry.dispatch_allow`
  (~2,080 ns), and `file_write.*` (~11.6–12.3 µs) baselines are
  unchanged.
- **No xmake plumbing change.** `oran_lib` globs `**.cpp` under
  `src/oran-tool/`, so the new `file_edit.cpp` is picked up without
  a target edit. `bench-tool` was already wired into `xmake/bench.lua`
  in slice 17; only its `main.cpp` adds the `register_tool_file_edit`
  hook.
- **Slice-version bump.** `kVersion` 18 → 19; the binary's CLI
  surface is unchanged. `xmake run orangutan --help` reports
  `orangutan v2.0.0-slice19`.

### Design Intent

**Why `file.edit` is the right next-slice candidate.** It is item
#1 on the refreshed `Next intended slice` list and the natural
extension of slice 17/18's registry + first-two-built-ins pattern.
`io::read_text_file` and `io::write_text_file` already exist with
the matching async signatures, so this slice does not pull in any
new IO machinery — it composes existing pieces. `file.search`
carries substantive mechanism (ripgrep-style structured matches),
so it warrants its own slice under the C14 ≤ 600-line guideline;
the approval-broker mediation also stays its own dedicated slice
because the broker is already implemented and the wiring change
should be uniform to every built-in rather than bolted onto
`file.edit`.

**Why `old_string` / `new_string` rather than patch-style.** The
design doc says `file.edit` "performs patch-style edits with
conflict detection". For an MVP slice that fits the C14 size
guideline, the `old_string` / `new_string` form Claude Code's own
Edit tool uses is far more tractable and matches what most LLMs
emit confidently. The "conflict detection" half of the design-doc
language is preserved here: uniqueness is enforced by default; the
`replace_all` flag is the explicit opt-out. A future slice can layer
a unified-diff parser on top — `file.edit` just needs to grow
another optional input shape, not a new dispatch entry.

**Why `not_found` for "old_string is absent" and `conflict` for
"old_string is ambiguous".** A missing substring is the same
category of failure as a missing file: the thing the call refers to
is not there. The error context carries `path`. An ambiguous
substring is the same category as `WriteMode::fail_if_exists`
collisions in `file.write` — the operation is well-formed but the
state of the target makes it refuse, and `match_count` is carried
in the error context so the model knows exactly how many matches
to disambiguate.

**Why empty `old_string` and `old==new` reject up front.** An empty
needle would match at every byte position and a no-op edit wastes
both an audit row and an IO round-trip. Rejecting these as
`invalid_argument` at the parse-validate stage is cheaper than
discovering the degeneracy after a 1 KiB read.

**Why the success text quotes the match count.** Matching
`file.write`'s `wrote N bytes to <path>` pattern: the agent loop
gets a concrete fact it can quote without parsing. Singular vs.
plural wording (`1 replacement` vs. `N replacements`) keeps the
text natural — LLMs that interpolate the success text into chat
responses don't have to special-case the count.

**Why one audit row, exactly as in slices 17/18.** Permission
decision is what audit records; the handler outcome is hook-bus
territory (still pending). Every built-in now demonstrates the
same invariant: the registry alone is responsible for the audit
record; the handler is responsible for the *action*. The
malformed-input table asserts this explicitly — all eight reject
paths still produce one `allow` audit row each because the
permission gate ran before the handler validated the payload.

**Why bench `unique_replace_vs_replace_all_many` instead of
`dispatch_vs_direct`.** The dispatch-vs-direct cost is already
documented by slice 17's `registry.lookup` / `registry.dispatch_allow`
A/B and slice 18's `file_write.*` numbers. The informative new
contrast for `file.edit` is "does the per-match substitution cost
matter against the shared read+write floor?" — and the measured
answer (~1 µs for 63 extra matches; ~16 ns per extra substitution)
is a documented baseline a future rope-based or in-place rewrite
would have to beat to justify itself.

### Files Modified

- `include/oran/tool/builtins.hpp` — new `kFileEditName` constant
  and `register_file_edit` declaration; refreshed module comment.
- `src/oran-tool/file_edit.cpp` — new TU; handler + registrar.
- `src/oran-tool/builtins.cpp` — `register_builtins` now also wires
  `file.edit` after `file.write`.
- `tests/tool/test_registry.cpp` — 7 new file.edit test cases
  (+66 assertions) plus an `edit_rule_set()` helper; existing
  `register_builtins` test now asserts a catalog of `{file.read,
  file.write, file.edit}`.
- `bench/tool/main.cpp` — registers the new
  `register_tool_file_edit` block.
- `bench/tool/scenarios/file_edit.cpp` — new scenario:
  unique-replace vs. replace-all-many dispatch on a 1 KiB seed.
- `bench/tool/README.md` — documents the new A/B.
- `src/oran-bootstrap/bootstrap.cpp` — `kVersion` 18 → 19.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 19, history pointer, library surfaces
  row for `oran-tool` (31 / 227), refreshed `Next intended slice`
  bullet (down to `file.search` + approval-broker + Anthropic +
  signal shutdown).
- `docs/QUALITY_SCORE.md` — Tool registry row refreshed with the
  third built-in's surface, schema, and bench numbers; Test
  framework row refreshed with the new `oran-tool` counts; Bench
  harness row refreshed with the new `file_edit` A/B numbers.
- `docs/ARCHITECTURE.md` — slice-status preamble bumped to
  2026-05-19 and now lists all three file built-ins; `oran-tool`
  inventory row promoted from "built-ins `file.read` and
  `file.write`" to "built-ins `file.read`, `file.write`, and
  `file.edit`".
- `docs/design-docs/tool-runtime.md` — Status box (2026-05-19)
  notes all three built-ins; `Capability::edit_file` is now
  consumed in production code.
- `docs/releases/feature-release-notes.md` — new top row for
  `oran-tool-file-edit`.
- `docs/histories/2026-05/20260517-2300-oran-tool-file-edit.md` —
  this file.

### Validation

- Commands run:
  - `xmake build oran-tool` — clean (5 TUs, ~9 s).
  - `xmake build test-tool && xmake run test-tool` — 31 cases /
    227 assertions, all green.
  - `xmake test` — all 9 buckets green
    (test-tool joins test-core/async/io/storage/config/permission/
    cli/bootstrap).
  - `xmake build bench-tool && xmake run bench-tool` — clean;
    measured `registry.lookup ~7.52 ns`,
    `registry.dispatch_allow ~2,080 ns`,
    `file_write.dispatch_truncate ~12,293 ns`,
    `file_write.dispatch_append ~11,588 ns`,
    `file_edit.dispatch_unique_replace ~15,056 ns`,
    `file_edit.dispatch_replace_all_many ~16,117 ns`.
  - `xmake build orangutan && xmake run orangutan -- --help` —
    prints the slice-19 banner; the CLI surface is unchanged.
  - `make ci` — docs scaffold, hygiene, docs-sync, STATUS
    freshness (latest history
    `20260517-2300-oran-tool-file-edit.md`), and action-pinning all
    pass.
- Tests added/changed: 7 new tool-bucket cases (+66 assertions).
- Bench impact: existing scenarios unchanged within noise; new
  `file_edit` scenarios baselined above.
- Compile-budget delta: one new TU in `oran-tool`
  (`file_edit.cpp` ~3 s for the nlohmann json include); one new
  TU in `bench-tool` (`file_edit.cpp` scenario); the new
  `<nlohmann/json.hpp>` include in `tests/tool/test_registry.cpp`
  was added by slice 18 and is amortised over the bucket's
  existing budget envelope.

### Follow-ups

- Issues to file: none.
- Tech-debt entries: none added, none closed (the "remaining file
  built-ins" bullet trims to `file.search`; the approval-broker
  mediation bullet is unchanged).
- Linked release note: 2026-05-17 `oran-tool-file-edit` row in
  `docs/releases/feature-release-notes.md`.
- Cross-references for future agents: when the agent loop
  (`oran-agent`) lands, the natural extraction is the same as for
  slices 17 and 18 — wire `bootstrap::RuntimeAssembly`'s
  `AuditSink` + a `permission::RuleSet` built via
  `materialize_rules` into a single `tool::DispatchContext`
  shared by every iteration. All three file built-ins now live in
  the catalog. When `tool::Runtime` lands, all three file
  built-ins should be migrated together onto the
  `runtime.workspace()` capability-gated handle so workspace
  confinement applies uniformly. When `file.search` lands, the
  catalog seed becomes `{file.read, file.write, file.edit,
  file.search}` and the long-form description of `file.edit`
  should be cross-referenced with `file.search` so an LLM can
  pick the right tool (search-then-edit vs. blind edit).
